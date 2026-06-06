#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#ifdef __EMSCRIPTEN__
extern "C" {
bool EmscriptenGetKeyState(int keyCode);
void EmscriptenGetMouseDelta(float* dx, float* dy);
bool EmscriptenGetMouseButton(int button);
void EmscriptenInitInput();
}
#endif

#include "TestDawnForwardRenderer.h"
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnCommandBuffer.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/SSAOPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/HZBPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/VelocityPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/LumenSSGIDawnPass.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Graphics/RenderProxy.h"
#include "Engine/Graphics/Material.h"
#include "Engine/Graphics/MaterialInstance.h"
#include "Engine/Graphics/RenderMesh.h"
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cmath>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <malloc/malloc.h>
#endif

#ifdef __EMSCRIPTEN__
static Engine_Test* g_engineTest = nullptr;
#endif

using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::handles;
namespace rhimath = primal::graphics::rhi::math;

// ============================================================
// Texture helpers (reused from TestDawnSponza)
// ============================================================

static ResourceHandle CreateTextureFromData(DawnDevice* device, int w, int h,
    const unsigned char* data, DataFormat format = DataFormat::RGBA8_UNorm) {
    u32 mipLevels = 1;
    { u32 maxDim = std::max((u32)w, (u32)h); while (maxDim > 1) { mipLevels++; maxDim /= 2; } }

    TextureDesc desc{};
    desc.size = {(u32)w, (u32)h, 1};
    desc.format = format;
    desc.type = TextureType::Texture2D;
    desc.mipLevels = mipLevels;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;

    ResourceHandle tex = device->CreateTexture(desc);
    if (tex == INVALID_RESOURCE) return INVALID_RESOURCE;

    device->UpdateTextureData(tex, data, 0, 0, 0, (u32)w, (u32)h, 1, 4 * w, 0);

    // Generate mip levels
    int curW = w, curH = h;
    std::vector<unsigned char> mipBuf;
    const unsigned char* srcData = data;
    for (u32 mip = 1; mip < mipLevels; ++mip) {
        int nextW = std::max(1, curW / 2);
        int nextH = std::max(1, curH / 2);
        mipBuf.resize(nextW * nextH * 4);
        for (int y = 0; y < nextH; ++y) {
            for (int x = 0; x < nextW; ++x) {
                int sx = x * 2, sy = y * 2;
                for (int c = 0; c < 4; ++c) {
                    mipBuf[(y * nextW + x) * 4 + c] =
                        (unsigned char)(((int)srcData[(sy * curW + sx) * 4 + c] +
                        (int)srcData[(sy * curW + std::min(sx+1, curW-1)) * 4 + c] +
                        (int)srcData[(std::min(sy+1, curH-1) * curW + sx) * 4 + c] +
                        (int)srcData[(std::min(sy+1, curH-1) * curW + std::min(sx+1, curW-1)) * 4 + c]) / 4);
                }
            }
        }
        device->UpdateTextureData(tex, mipBuf.data(), 0, 0, 0, (u32)nextW, (u32)nextH, 1, 4 * nextW, mip);
        srcData = mipBuf.data();
        curW = nextW;
        curH = nextH;
    }
    return tex;
}

static ResourceHandle LoadTextureFromFile(DawnDevice* device, const std::string& path,
    DataFormat format = DataFormat::RGBA8_UNorm) {
    int width, height, channels;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data) return INVALID_RESOURCE;
    ResourceHandle tex = CreateTextureFromData(device, width, height, data, format);
    stbi_image_free(data);
    return tex;
}

static std::string ResolveTexturePath(const std::string& base, const std::string& filename) {
    if (filename.empty()) return "";
    std::string path = base + filename;
    std::ifstream f(path);
    if (f.is_open()) return path;
    // Try models/Sponza/
    path = base + "models/Sponza/" + filename;
    f.open(path);
    if (f.is_open()) return path;
    // Try textures/
    path = base + "textures/" + filename;
    f.open(path);
    if (f.is_open()) return path;
    return "";
}

static std::string LoadShaderSource(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
#ifdef __EMSCRIPTEN__
        std::ifstream file2(path);
#else
        std::ifstream file2("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + path);
#endif
        if (!file2.is_open()) return "";
        std::stringstream buf;
        buf << file2.rdbuf();
        return buf.str();
    }
    std::stringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

// ============================================================
// Engine_Test
// ============================================================

Engine_Test::~Engine_Test() {
    shutdown();
}

bool Engine_Test::initialize() {
    std::cout << "[TestDawnForwardRenderer] Initializing..." << std::endl;

    // Create window
    platform::window_init_info windowInfo{
        nullptr, nullptr,
        "TestDawnForwardRenderer",
        100, 100, (s32)width_, (s32)height_
    };
    window_ = platform::create_window(&windowInfo);
    if (!window_.is_valid()) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }

    // Create Dawn device
    rhi::DeviceDesc deviceDesc;
    deviceDesc.platform = rhi::RHIPlatform::Dawn;
    deviceDesc.enableDebug = true;
    deviceDesc.enableValidation = false;
    deviceDesc.maxFramesInFlight = kFrameCount;
    device_ = new rhi::DawnDevice(deviceDesc);
    if (!device_ || !device_->Initialize()) {
        std::cerr << "Failed to create Dawn device" << std::endl;
        return false;
    }
    std::cout << "[TestDawnForwardRenderer] Dawn device created" << std::endl;

    // Create persistent depth texture (reused across frames)
    CreateDepthTexture();

    // Create persistent HDR render target for post-processing
    hdrDesc_.size = {width_, height_, 1};
    hdrDesc_.format = rhi::DataFormat::RGBA16_Float;
    hdrDesc_.type = rhi::TextureType::Texture2D;
    hdrDesc_.mipLevels = 1;
    hdrDesc_.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    hdrTexture_ = device_->CreateTexture(hdrDesc_);

    // Create render graph for post-processing
    renderGraph_ = std::make_unique<rendergraph::RenderGraph>(*device_);

    // Create swapchain
    rhi::SwapChainDesc scDesc{};
    scDesc.window = window_.handle();
    scDesc.width = width_;
    scDesc.height = height_;
    scDesc.format = rhi::DataFormat::BGRA8_UNorm;
    scDesc.bufferCount = kFrameCount;
    swapchain_ = device_->CreateSwapChain(scDesc);
    if (!swapchain_) {
        std::cerr << "Failed to create swapchain" << std::endl;
        return false;
    }
    std::cout << "[TestDawnForwardRenderer] Swapchain created" << std::endl;

    // Initialize ForwardRenderer
    if (!forwardRenderer_.Initialize(device_)) {
        std::cerr << "Failed to initialize ForwardRenderer" << std::endl;
        return false;
    }
    std::cout << "[TestDawnForwardRenderer] ForwardRenderer initialized" << std::endl;

#ifndef __EMSCRIPTEN__
    // Create shadow resources before LoadSponzaScene (shadow depth texture + sampler)
    std::cout << "[TestDawnForwardRenderer] Creating shadow resources..." << std::endl;
    CreateShadowResources();
    std::cout << "[TestDawnForwardRenderer] Shadow resources created" << std::endl;
#endif

    // Load Sponza scene
    if (!LoadSponzaScene()) {
        std::cerr << "Failed to load Sponza scene" << std::endl;
        return false;
    }

    // Setup scene lights
    RenderLight dirLight;
    dirLight.type = LightType::Directional;
    dirLight.direction = primal::math::v3{0.5f, -0.7f, 0.3f};
    dirLight.color = primal::math::v3{1.0f, 0.95f, 0.9f};
    dirLight.intensity = 3.0f;
    scene_.AddLight(dirLight);

    // Setup camera
    UpdateCameraView();

    rhimath::m4x4 projMat = rhimath::CreatePerspectiveMatrix(
        60.0f * rhimath::constants::DEG_TO_RAD,
        static_cast<float>(width_) / static_cast<float>(height_), 0.1f, 100.0f);
    view_.SetProjectionMatrix(projMat);

    rhi::ViewportDesc viewport;
    viewport.topLeft = {0.0f, 0.0f};
    viewport.size = {static_cast<float>(width_), static_cast<float>(height_)};
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    view_.SetViewport(viewport);

    rhi::Rect scissor;
    scissor.offset = {0, 0};
    scissor.extent = {width_, height_};
    view_.SetScissor(scissor);

    // Cull to populate visible proxies
    view_.Cull(scene_);

#ifdef __EMSCRIPTEN__
    // Diagnostic: check AABB validity and frustum planes
    {
        u32 validAABB = 0, invalidAABB = 0;
        for (const auto& info : sceneMeshInfos_) {
            if (info.mesh && info.mesh->IsValid()) validAABB++;
            else invalidAABB++;
        }
        std::cout << "[TestDawnFR] AABB stats: " << validAABB << " valid, " << invalidAABB << " invalid" << std::endl;
    }
    if (view_.GetVisibleProxies().empty()) {
        std::cout << "[TestDawnFR] WARNING: 0 visible proxies, bypassing frustum culling" << std::endl;
        // Force all proxies visible to diagnose rendering
        auto& proxies = scene_.GetProxies();
        auto& visible = const_cast<utl::vector<const RenderProxy*>&>(view_.GetVisibleProxies());
        visible.clear();
        for (const auto& p : proxies) {
            visible.push_back(&p);
        }
    }
#endif

    std::cout << "[TestDawnForwardRenderer] Scene loaded: " << sceneMeshInfos_.size()
              << " meshes, " << scene_.GetProxies().size() << " proxies, "
              << view_.GetVisibleProxies().size() << " visible" << std::endl;

#ifdef __EMSCRIPTEN__
    g_engineTest = this;
#endif

    return true;
}

bool Engine_Test::LoadSponzaScene() {
#ifdef __EMSCRIPTEN__
    std::string baseDir = "assets/";
#else
    std::string baseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
#endif
    std::string modelPath = baseDir + "Sponza_process_rebuild.model";
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        modelPath = baseDir + "Sponza.model";
        file.open(modelPath, std::ios::binary | std::ios::ate);
    }
    if (!file.is_open()) {
        std::cerr << "[TestDawnFR] Failed to open Sponza model" << std::endl;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) {
        std::cerr << "[TestDawnFR] Failed to read Sponza model" << std::endl;
        return false;
    }

    SceneDataAdapter adapter;
    sceneMeshInfos_ = adapter.LoadRenderItemData(device_, buffer.data(), (u32)buffer.size());
    if (sceneMeshInfos_.empty()) {
        std::cerr << "[TestDawnFR] No meshes loaded" << std::endl;
        return false;
    }
    std::cout << "[TestDawnFR] Loaded " << sceneMeshInfos_.size() << " meshes" << std::endl;

    // Load Dawn-compatible PBR shader (no shadow CombinedImageSampler bindings)
    std::cout << "[TestDawnFR] Loading shader..." << std::endl;
    std::string shaderSource = LoadShaderSource("Engine/Graphics/Dawn/shaders/ForwardPBR_NoShadow.wgsl");
    std::cout << "[TestDawnFR] Shader source: " << (shaderSource.empty() ? "EMPTY" : "OK") << " size=" << shaderSource.size() << std::endl;
    if (shaderSource.empty()) {
        std::cerr << "[TestDawnFR] Failed to load ForwardPBR_NoShadow.wgsl" << std::endl;
        return false;
    }

    // Test: verify shader compiles on Dawn
    auto testVS = device_->CreateShader(shaderSource.data(), shaderSource.size() + 1, ShaderStage::Vertex, "vertexMain");
    auto testFS = device_->CreateShader(shaderSource.data(), shaderSource.size() + 1, ShaderStage::Pixel, "fragmentMain");
    std::cout << "[TestDawnFR] Shader compilation: VS=" << testVS << " FS=" << testFS << std::endl;

    // Create shared Material for all meshes
    auto material = std::make_shared<Material>();
    // Include null terminator for WGSL source text
    material->SetShader(ShaderStage::Vertex, shaderSource.data(), shaderSource.size() + 1, "vertexMain");
    material->SetShader(ShaderStage::Pixel, shaderSource.data(), shaderSource.size() + 1, "fragmentMain");

    // Vertex attributes (32-byte interleaved: pos + colorTSign + packedNormal + packedTangent + uv)
    utl::vector<VertexInputAttribute> attrs(5);
    attrs[0] = {0, 0, DataFormat::RGB32_Float, 0};   // position
    attrs[1] = {1, 0, DataFormat::R32_UInt, 12};      // colorTSign
    attrs[2] = {2, 0, DataFormat::R32_UInt, 16};      // packedNormal
    attrs[3] = {3, 0, DataFormat::R32_UInt, 20};      // packedTangent
    attrs[4] = {4, 0, DataFormat::RG32_Float, 24};    // uv
    material->SetVertexAttributes(attrs);

    utl::vector<VertexInputBinding> bindings(1);
    bindings[0] = {0, 32, true};
    material->SetVertexBindings(bindings);

    // Depth/stencil state
    DepthStencilState dsState{};
    dsState.enableDepthTest = true;
    dsState.enableDepthWrite = true;
    dsState.depthFunc = ComparisonFunc::Less;
    material->SetDepthStencilState(dsState);

    // Rasterizer state
    RasterizerState rasterState{};
    rasterState.cullMode = CullMode::None;
    rasterState.fillMode = FillMode::Solid;
    material->SetRasterizerState(rasterState);

    // Render target format — match actual render target
    utl::vector<DataFormat> rtFormats(1);
#ifdef __EMSCRIPTEN__
    rtFormats[0] = DataFormat::BGRA8_UNorm;  // Render directly to backbuffer
#else
    rtFormats[0] = DataFormat::RGBA16_Float; // Render to HDR texture
#endif
    material->SetRenderTargetFormats(rtFormats, DataFormat::D32_Float);

    // Pipeline layout: 3 descriptor set layouts matching ForwardPBR_NoShadow.wgsl
    // Group 0 (Global Dawn): binding 11 = GlobalShaderData(UB), binding 12 = ForwardLightBuffer(UB)
    // Group 1 (PerObject): binding 10 = PerObjectData(DynamicUB)
    // Group 2 (Material): binding 0 = albedo, 1 = normal, 2 = ORM, 3 = sampler

    // Use ForwardRenderer's Dawn-compatible global layout (2 bindings: 11+12 only)
    auto globalLayout = forwardRenderer_.GetGlobalDescriptorSetLayout();

    // Material descriptor set layout (Group 2)
    DescriptorSetLayoutBinding matBindings[4]{};
    matBindings[0].binding = 0;
    matBindings[0].descriptorType = DescriptorType::SampledImage;
    matBindings[0].descriptorCount = 1;
    matBindings[0].stageFlags = ShaderStage::Pixel;

    matBindings[1].binding = 1;
    matBindings[1].descriptorType = DescriptorType::SampledImage;
    matBindings[1].descriptorCount = 1;
    matBindings[1].stageFlags = ShaderStage::Pixel;

    matBindings[2].binding = 2;
    matBindings[2].descriptorType = DescriptorType::SampledImage;
    matBindings[2].descriptorCount = 1;
    matBindings[2].stageFlags = ShaderStage::Pixel;

    matBindings[3].binding = 3;
    matBindings[3].descriptorType = DescriptorType::Sampler;
    matBindings[3].descriptorCount = 1;
    matBindings[3].stageFlags = ShaderStage::Pixel;

    DescriptorSetLayoutDesc matLayoutDesc{};
    matLayoutDesc.bindings = matBindings;
    matLayoutDesc.bindingCount = 4;
    materialSetLayout_ = device_->CreateDescriptorSetLayout(matLayoutDesc);
    if (materialSetLayout_ == INVALID_DESCRIPTOR_SET_LAYOUT) {
        std::cerr << "[TestDawnFR] Failed to create material set layout" << std::endl;
        return false;
    }
    material->SetDescriptorSetLayout(materialSetLayout_);

    // Pipeline layout: 3 groups (Global with shadow bindings, PerObject, Material)
    auto perObjectLayout = forwardRenderer_.GetPerObjectDescriptorSetLayout();
    DescriptorSetLayoutHandle setLayouts[3] = { globalLayout, perObjectLayout, materialSetLayout_ };

    PipelineLayoutDesc plDesc{};
    plDesc.setLayouts = setLayouts;
    plDesc.setLayoutCount = 3;
    pipelineLayout_ = device_->CreatePipelineLayout(plDesc);
    if (pipelineLayout_ == INVALID_PIPELINE_LAYOUT) {
        std::cerr << "[TestDawnFR] Failed to create pipeline layout" << std::endl;
        return false;
    }
    material->SetPipelineLayout(pipelineLayout_);

    // Create sampler for material textures
    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Wrap;
    samplerDesc.addressV = TextureAddressMode::Wrap;
    samplerDesc.addressW = TextureAddressMode::Wrap;
    samplerDesc.comparisonFunc = ComparisonFunc::Never; // Must be Never for non-comparison (filtering) sampler on WebGPU
    materialSampler_ = device_->CreateSampler(samplerDesc);

    // Create MaterialInstance for each mesh with textures
    std::string textureBase = baseDir;
    u32 texLoaded = 0, texFailed = 0;

    std::cout << "[TestDawnFR] Creating material instances for " << sceneMeshInfos_.size() << " meshes..." << std::endl;

    for (u32 i = 0; i < sceneMeshInfos_.size(); ++i) {
        auto& meshInfo = sceneMeshInfos_[i];
        meshInfo.material = material; // Shared material

        auto matInst = std::make_shared<MaterialInstance>(material.get());
        if (!matInst->Initialize(device_)) {
            std::cerr << "[TestDawnFR] Failed to init MaterialInstance " << i << std::endl;
            matInst = std::make_shared<MaterialInstance>(material.get());
        }

        // Load diffuse texture
        ResourceHandle diffuseTex = INVALID_RESOURCE;
        std::string diffusePath = ResolveTexturePath(textureBase, meshInfo.diffuseTexturePath);
        if (!diffusePath.empty()) {
            diffuseTex = LoadTextureFromFile(device_, diffusePath);
        }
        if (diffuseTex == INVALID_RESOURCE) {
            unsigned char white[4] = {255, 255, 255, 255};
            diffuseTex = CreateTextureFromData(device_, 1, 1, white);
            texFailed++;
        } else {
            texLoaded++;
        }

        // Load normal texture
        ResourceHandle normalTex = INVALID_RESOURCE;
        std::string normalPath = ResolveTexturePath(textureBase, meshInfo.normalTexturePath);
        if (!normalPath.empty()) {
            normalTex = LoadTextureFromFile(device_, normalPath);
        }
        if (normalTex == INVALID_RESOURCE) {
            unsigned char flatNormal[4] = {128, 128, 255, 255};
            normalTex = CreateTextureFromData(device_, 1, 1, flatNormal);
        }

        // Load ORM texture
        ResourceHandle ormTex = INVALID_RESOURCE;
        std::string ormPath = ResolveTexturePath(textureBase, meshInfo.ormTexturePath);
        if (!ormPath.empty()) {
            ormTex = LoadTextureFromFile(device_, ormPath);
        }
        if (ormTex == INVALID_RESOURCE) {
            unsigned char defaultORM[4] = {255, 128, 0, 255}; // AO=1, roughness=0.5, metallic=0
            ormTex = CreateTextureFromData(device_, 1, 1, defaultORM);
        }

        // Set textures on MaterialInstance
        matInst->SetTexture(0, diffuseTex);
        matInst->SetTexture(1, normalTex);
        matInst->SetTexture(2, ormTex);
        matInst->SetSampler(3, materialSampler_);
        matInst->Update(device_);

        meshInfo.materialInstance = matInst;

        // Create RenderProxy
        RenderProxy proxy;
        proxy.meshId = meshInfo.meshEntityId;
        proxy.materialId = meshInfo.meshEntityId; // Use entity ID as material key
        proxy.entityId = meshInfo.meshEntityId;

        // Identity transform for Sponza
        // Identity transform
        proxy.transform = rhimath::MatrixIdentity();

        // Compute AABB from mesh if available
        if (meshInfo.mesh && meshInfo.mesh->IsValid()) {
            proxy.worldAABB = meshInfo.mesh->GetLocalAABB();
        }

        scene_.AddProxy(proxy);

        // Add to materials map (keyed by materialId = entityId)
        materials_[proxy.materialId] = matInst;

        if ((i + 1) % 50 == 0) {
            std::cout << "[TestDawnFR] Processed " << (i + 1) << "/" << sceneMeshInfos_.size() << " meshes" << std::endl;
        }
    }

    std::cout << "[TestDawnFR] Textures: " << texLoaded << " loaded, " << texFailed << " fallback" << std::endl;
    return true;
}

#ifdef __EMSCRIPTEN__
void Engine_Test::ReloadTextures() {
    if (texturesReloaded_) return;

    std::string textureBase = "assets/";
    u32 reloaded = 0, failed = 0;

    for (u32 i = 0; i < sceneMeshInfos_.size(); ++i) {
        auto& meshInfo = sceneMeshInfos_[i];
        if (!meshInfo.materialInstance) continue;

        // Load diffuse
        ResourceHandle diffuseTex = INVALID_RESOURCE;
        std::string diffusePath = ResolveTexturePath(textureBase, meshInfo.diffuseTexturePath);
        if (!diffusePath.empty()) {
            diffuseTex = LoadTextureFromFile(device_, diffusePath);
        }
        if (diffuseTex == INVALID_RESOURCE) {
            failed++;
            continue; // skip if still not available
        }

        // Load normal
        ResourceHandle normalTex = INVALID_RESOURCE;
        std::string normalPath = ResolveTexturePath(textureBase, meshInfo.normalTexturePath);
        if (!normalPath.empty()) {
            normalTex = LoadTextureFromFile(device_, normalPath);
        }
        if (normalTex == INVALID_RESOURCE) {
            unsigned char flatNormal[4] = {128, 128, 255, 255};
            normalTex = CreateTextureFromData(device_, 1, 1, flatNormal);
        }

        // Load ORM
        ResourceHandle ormTex = INVALID_RESOURCE;
        std::string ormPath = ResolveTexturePath(textureBase, meshInfo.ormTexturePath);
        if (!ormPath.empty()) {
            ormTex = LoadTextureFromFile(device_, ormPath);
        }
        if (ormTex == INVALID_RESOURCE) {
            unsigned char defaultORM[4] = {255, 128, 0, 255};
            ormTex = CreateTextureFromData(device_, 1, 1, defaultORM);
        }

        // Update all frame indices' descriptor sets
        for (u32 fi = 0; fi < kFrameCount; ++fi) {
            meshInfo.materialInstance->SetCurrentFrame(fi);
            meshInfo.materialInstance->SetTexture(0, diffuseTex);
            meshInfo.materialInstance->SetTexture(1, normalTex);
            meshInfo.materialInstance->SetTexture(2, ormTex);
            meshInfo.materialInstance->SetSampler(3, materialSampler_);
            meshInfo.materialInstance->Update(device_);
        }
        reloaded++;
    }

    std::cout << "[TestDawnFR] Texture reload: " << reloaded << "/" << sceneMeshInfos_.size() << std::endl;
    texturesReloaded_ = (reloaded > 0);
}
#endif

void Engine_Test::RenderFrame() {
    if (!device_ || !swapchain_ || shuttingDown_) return;

    device_->BeginFrame();

    auto now = std::chrono::steady_clock::now();
    float dt = lastFrameTime_.time_since_epoch().count() > 0
        ? std::chrono::duration<float>(now - lastFrameTime_).count()
        : 1.0f / 60.0f;
    lastFrameTime_ = now;
    if (dt > 0.1f) dt = 1.0f / 60.0f;

    UpdateCamera(dt);

#ifdef __EMSCRIPTEN__
    // Poll for texture reload after ~2 seconds (frame 120), retry every 120 frames
    if (!texturesReloaded_ && totalFrames_ >= 120 && totalFrames_ % 120 == 0) {
        ReloadTextures();
    }
#endif

    u32 imageIndex = 0;
    if (!swapchain_->AcquireNextImage(&imageIndex)) {
        device_->EndFrame();
        frameIndex_++;
        return;
    }

    rhi::ResourceHandle backBuffer = swapchain_->GetBackBuffer(imageIndex);
    if (backBuffer == rhi::handles::INVALID_RESOURCE) return;

    u32 fi = frameIndex_ % kFrameCount;
    forwardRenderer_.SetTime(dt, totalFrames_ / 60.0f, frameIndex_);

#ifdef __EMSCRIPTEN__
    // ============================================================
    // Simplified render path for WASM: forward pass → present
    // No render graph, no post-processing (HZB/SSGI/SSAO/Bloom/TM)
    // ============================================================

    if (totalFrames_ == 0) std::cout << "[TestDawnFR] First frame begin" << std::endl;

    if (cmdBuffer_ == rhi::handles::INVALID_COMMAND_BUFFER) {
        cmdBuffer_ = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    }
    rhi::RHICommandBuffer* cmd = device_->GetCommandBuffer(cmdBuffer_);
    if (cmd) {
        cmd->Reset();
        if (cmd->Begin()) {
            if (totalFrames_ == 0) std::cout << "[TestDawnFR] Calling ForwardRenderer::Render" << std::endl;
            // Forward pass directly to backbuffer + depth
            forwardRenderer_.Render(cmd, scene_, view_, backBuffer, depthTexture_, materials_, fi, width_, height_);
            if (totalFrames_ == 0) std::cout << "[TestDawnFR] ForwardRenderer::Render done" << std::endl;
            cmd->End();
        }
        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdBuffer_;
        if (totalFrames_ == 0) std::cout << "[TestDawnFR] Submitting" << std::endl;
        device_->Submit(submitInfo);
        if (totalFrames_ == 0) std::cout << "[TestDawnFR] Submit done" << std::endl;
    }

    if (totalFrames_ == 0) std::cout << "[TestDawnFR] Presenting" << std::endl;
    swapchain_->Present(rhi::handles::INVALID_SYNC);
    if (totalFrames_ == 0) std::cout << "[TestDawnFR] Present done" << std::endl;
    device_->EndFrame();
    frameIndex_++;
    totalFrames_++;

    if (totalFrames_ % 60 == 0) {
        std::cout << "[TestDawnFR] Frame " << totalFrames_ << std::endl;
    }
#else
    // 1. Setup render graph FIRST so SSAO/Bloom/ToneMapping resources are created
    //    before any command buffers. Dawn can corrupt heap if resources are created
    //    between two command buffer recordings.
    renderGraph_->Clear();

    auto hdrRG = renderGraph_->ImportTexture("HDR", hdrTexture_, hdrDesc_);

    rhi::TextureDesc depthDescForRG;
    depthDescForRG.size = {width_, height_, 1};
    depthDescForRG.format = rhi::DataFormat::D32_Float;
    depthDescForRG.type = rhi::TextureType::Texture2D;
    depthDescForRG.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    auto depthRG = renderGraph_->ImportTexture("Depth", depthTexture_, depthDescForRG);

    auto invProj = rhimath::Inverse(view_.GetProjectionMatrix());

    // HZB generation from depth buffer
    const auto& hzbOut = PostProcess::AddHZBPass(*renderGraph_, depthRG, width_, height_);
    auto hzbHandle = hzbOut.hzbTexture;

    // Velocity buffer (static camera → zero velocity, but needed for temporal pass)
    auto viewProj = view_.GetViewMatrix() * view_.GetProjectionMatrix();
    const auto& velOut = PostProcess::AddVelocityPass(*renderGraph_, depthRG, width_, height_, fi,
        viewProj, viewProj, invProj);
    auto velHandle = velOut.velocityTexture;

    // Lumen SSGI (4 sub-passes: trace → denoise → filter → temporal)
    const auto& ssgiOut = PostProcess::AddLumenSSGIPass(*renderGraph_, depthRG, hzbHandle,
        velHandle, hdrRG, width_, height_, fi, view_.GetProjectionMatrix(), invProj);
    auto ssgiHandle = ssgiOut.ssgiOutput;

    const auto& ssaoOut = graphics::PostProcess::AddSSAOPass(*renderGraph_, depthRG, width_, height_, fi, view_.GetProjectionMatrix(), invProj);
    auto ssaoAOHandle = ssaoOut.ssaoOutput;

    const auto& bloomOut = PostProcess::AddBloomPass(*renderGraph_, hdrRG, fi);
    auto bloomHandle = bloomOut.bloomOutput;

    const auto& tonemapOut = PostProcess::AddToneMappingPass(*renderGraph_, hdrRG, bloomHandle, ssaoAOHandle, ssgiHandle, fi);
    auto tonemapOutput = tonemapOut.output;

    // Present: blit tonemapped output → backbuffer
    rhi::TextureDesc bbDesc;
    bbDesc.size = {width_, height_, 1};
    bbDesc.format = rhi::DataFormat::BGRA8_UNorm;
    bbDesc.type = rhi::TextureType::Texture2D;
    bbDesc.usage = rhi::TextureUsage::RenderTarget;
    auto bbRG = renderGraph_->ImportTexture("BackBuffer", backBuffer, bbDesc);

    struct PresentData { rendergraph::RGResourceHandle input; rendergraph::RGResourceHandle output; };

    // Blit pipeline (static, created once during first setup)
    static rhi::PipelineHandle s_presentPipeline = rhi::handles::INVALID_PIPELINE;
    static rhi::PipelineLayoutHandle s_presentLayout = rhi::handles::INVALID_PIPELINE_LAYOUT;
    static rhi::DescriptorSetLayoutHandle s_presentDSL = rhi::handles::INVALID_RESOURCE;
    static rhi::SamplerHandle s_presentSampler = rhi::handles::INVALID_SAMPLER;

    renderGraph_->AddPass<PresentData>("PresentPass", rendergraph::RGPassType::Graphics, rendergraph::RGPassCategory::Present,
        [&](PresentData& data, rendergraph::RenderGraphBuilder& builder) {
            data.input = tonemapOutput;
            data.output = bbRG;
            builder.Read(data.input, rhi::ResourceState::ShaderResource);
            builder.Write(data.output, rhi::ResourceState::RenderTarget);
            builder.SideEffect();

            // Create Blit pipeline during setup (not during execute)
            if (s_presentPipeline == rhi::handles::INVALID_PIPELINE) {
                auto& dev = builder.GetGraph().GetDevice();
                auto platform = dev.GetPlatform();
                std::string path = utils::ShaderRegistry::GetShaderPath(platform, "Blit");
                std::ifstream f(path);
                std::stringstream buf;
                if (f.is_open()) buf << f.rdbuf();
                std::string src = buf.str();
                if (src.empty()) return;

                auto vs = dev.CreateShader(src.data(), src.size(), rhi::ShaderStage::Vertex, "blit_vs");
                auto fs = dev.CreateShader(src.data(), src.size(), rhi::ShaderStage::Pixel, "blit_fs");
                if (vs == rhi::handles::INVALID_SHADER || fs == rhi::handles::INVALID_SHADER) return;

                rhi::DescriptorSetLayoutBinding bindings[2]{};
                bindings[0] = {0, rhi::DescriptorType::SampledImage, 1, rhi::ShaderStage::Pixel};
                bindings[1] = {1, rhi::DescriptorType::Sampler, 1, rhi::ShaderStage::Pixel};
                rhi::DescriptorSetLayoutDesc dslDesc{2, bindings};
                s_presentDSL = dev.CreateDescriptorSetLayout(dslDesc);

                rhi::SamplerDesc sDesc;
                sDesc.minFilter = rhi::FilterMode::Linear;
                sDesc.magFilter = rhi::FilterMode::Linear;
                sDesc.addressU = rhi::TextureAddressMode::Clamp;
                sDesc.addressV = rhi::TextureAddressMode::Clamp;
                sDesc.addressW = rhi::TextureAddressMode::Clamp;
                sDesc.comparisonFunc = rhi::ComparisonFunc::Never;
                s_presentSampler = dev.CreateSampler(sDesc);

                rhi::PipelineLayoutDesc plDesc;
                plDesc.setLayoutCount = 1;
                plDesc.setLayouts = &s_presentDSL;
                s_presentLayout = dev.CreatePipelineLayout(plDesc);

                rhi::GraphicsPipelineDesc gpDesc;
                gpDesc.vertexShader = vs;
                gpDesc.pixelShader = fs;
                gpDesc.layout = s_presentLayout;
                gpDesc.topology = rhi::PrimitiveTopology::TriangleList;
                gpDesc.renderTargetCount = 1;
                gpDesc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm;
                gpDesc.enableDepthTest = false;
                gpDesc.enableDepthWrite = false;
                gpDesc.cullMode = rhi::CullMode::None;
                s_presentPipeline = dev.CreateGraphicsPipeline(gpDesc);
            }
        },
        [this](const PresentData& data, rendergraph::RenderGraphContext& context) {
            if (s_presentPipeline == rhi::handles::INVALID_PIPELINE) return;

            auto& dev = context.graph->GetDevice();
            auto* inputRes = context.graph->GetResource(data.input);
            auto* outputRes = context.graph->GetResource(data.output);
            if (!inputRes || !outputRes) return;

            rhi::ResourceHandle inputHandle = inputRes->GetPhysicalHandle();

            // Use physical handle directly — updateDescriptorSetsImpl resolves via GetDefaultView()
            rhi::DescriptorSetHandle ds = rhi::handles::INVALID_RESOURCE;
            if (inputHandle != rhi::handles::INVALID_RESOURCE) {
                rhi::DescriptorSetDesc dsDesc;
                dsDesc.layout = s_presentDSL;
                ds = dev.CreateDescriptorSet(dsDesc);
                if (ds != rhi::handles::INVALID_RESOURCE) {
                    rhi::DescriptorImageInfo img[2];
                    img[0].imageView = inputHandle;
                    img[0].sampler = s_presentSampler;
                    img[1].imageView = inputHandle;
                    img[1].sampler = s_presentSampler;
                    rhi::WriteDescriptorSet w[2];
                    w[0] = {ds, 0, 0, 1, rhi::DescriptorType::SampledImage, &img[0]};
                    w[1] = {ds, 1, 0, 1, rhi::DescriptorType::Sampler, &img[1]};
                    dev.UpdateDescriptorSets(2, w);
                }
            }

            rhi::RenderPassDesc passDesc;
            passDesc.colorAttachments.resize(1);
            passDesc.colorAttachments[0].texture = outputRes->GetPhysicalHandle();
            passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear;
            passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;

            rhi::ViewportDesc vp;
            vp.size = {(float)width_, (float)height_};
            vp.minDepth = 0.0f; vp.maxDepth = 1.0f;

            context.cmdBuffer->BeginRenderPass(passDesc);
            context.cmdBuffer->SetViewport(vp);
            context.cmdBuffer->SetScissor({{0, 0}, {width_, height_}});
            context.cmdBuffer->BindGraphicsPipeline(s_presentPipeline);
            if (ds != rhi::handles::INVALID_RESOURCE)
                context.cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, s_presentLayout, 0, 1, &ds, 0, nullptr);
            context.cmdBuffer->Draw(3, 0, 1, 0);
            context.cmdBuffer->EndRenderPass();

            // Deferred destroy descriptor set (no per-frame texture views created)
            static constexpr u32 MAX_FRAMES = 3;
            static utl::vector<rhi::DescriptorSetHandle> s_DeferredDS[MAX_FRAMES];
            static u32 s_FrameIdx = 0;
            if (ds != rhi::handles::INVALID_RESOURCE) {
                s_DeferredDS[s_FrameIdx % MAX_FRAMES].push_back(ds);
            }
            s_FrameIdx++;
            if (s_FrameIdx >= MAX_FRAMES) {
                u32 fi = s_FrameIdx % MAX_FRAMES;
                for (auto& h : s_DeferredDS[fi]) dev.DestroyDescriptorSet(h);
                s_DeferredDS[fi].clear();
            }
        }
    );

    renderGraph_->Compile();

    // 2. Submit 1: Shadow + Forward (heavy draw calls)
    if (cmdBuffer_ == rhi::handles::INVALID_COMMAND_BUFFER) {
        cmdBuffer_ = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    }
    rhi::RHICommandBuffer* cmd = device_->GetCommandBuffer(cmdBuffer_);
    if (cmd) {
        cmd->Reset();
        if (cmd->Begin()) {
            // 1. Shadow pass (depth-only from light POV)
            RenderShadowPass(cmd);

            // 2. Pass light VP to ForwardRenderer (writes viewProjections[0] in light buffer)
            forwardRenderer_.SetDawnShadowLightVP(lightVP_);

            // 3. Forward pass — shadow bindings are in Group 0 (global set)
            forwardRenderer_.Render(cmd, scene_, view_, hdrTexture_, depthTexture_, materials_, fi, width_, height_);
            cmd->End();
        }
        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdBuffer_;
        device_->Submit(submitInfo);
    }

    // 3. Submit 2: Post-processing (render graph)
    if (postCmdBuffer_ == rhi::handles::INVALID_COMMAND_BUFFER) {
        postCmdBuffer_ = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    }
    rhi::RHICommandBuffer* postCmd = device_->GetCommandBuffer(postCmdBuffer_);
    if (postCmd) {
        postCmd->Reset();
        if (postCmd->Begin()) {
            renderGraph_->Execute(postCmd);
            postCmd->End();
        }
        rhi::QueueSubmitInfo postSubmitInfo{};
        postSubmitInfo.cmdBuffer = postCmdBuffer_;
        device_->Submit(postSubmitInfo);
    }

    swapchain_->Present(rhi::handles::INVALID_SYNC);
    device_->EndFrame();
    frameIndex_++;
    totalFrames_++;

    if (totalFrames_ % 60 == 0) {
        std::cout << "[TestDawnForwardRenderer] Frame " << totalFrames_ << std::endl;
    }
#endif // __EMSCRIPTEN__
}

void Engine_Test::UpdateCamera(float dt) {
#ifdef __APPLE__
    float speed = cameraSpeed_ * dt;

    auto keyPressed = [](uint16_t keyCode) -> bool {
        return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, keyCode);
    };

    // WASD: W=13, A=0, S=1, D=2 (macOS virtual key codes)
    if (keyPressed(13)) { // W - forward
        cameraPos_.x -= sinf(cameraYaw_) * speed;
        cameraPos_.z -= cosf(cameraYaw_) * speed;
    }
    if (keyPressed(1)) { // S - backward
        cameraPos_.x += sinf(cameraYaw_) * speed;
        cameraPos_.z += cosf(cameraYaw_) * speed;
    }
    if (keyPressed(0)) { // A - left
        cameraPos_.x -= cosf(cameraYaw_) * speed;
        cameraPos_.z += sinf(cameraYaw_) * speed;
    }
    if (keyPressed(2)) { // D - right
        cameraPos_.x += cosf(cameraYaw_) * speed;
        cameraPos_.z -= sinf(cameraYaw_) * speed;
    }
    if (keyPressed(12)) cameraPos_.y -= speed;  // Q - down
    if (keyPressed(14)) cameraPos_.y += speed;  // E - up

    // Arrow keys for camera rotation: Left=123, Right=124, Up=126, Down=125
    if (keyPressed(123)) cameraYaw_ += 2.0f * dt;
    if (keyPressed(124)) cameraYaw_ -= 2.0f * dt;
    if (keyPressed(126)) cameraPitch_ += 2.0f * dt;
    if (keyPressed(125)) cameraPitch_ -= 2.0f * dt;

    // ESC to quit
    if (keyPressed(53)) {
        shuttingDown_ = true;
        if (runLoop_) CFRunLoopStop(runLoop_);
        return;
    }
#endif

#ifdef __EMSCRIPTEN__
    float speed = cameraSpeed_ * dt;

    // WASD using JS key codes: W=87, A=65, S=83, D=68
    if (EmscriptenGetKeyState(87)) { // W
        cameraPos_.x -= sinf(cameraYaw_) * speed;
        cameraPos_.z -= cosf(cameraYaw_) * speed;
    }
    if (EmscriptenGetKeyState(83)) { // S
        cameraPos_.x += sinf(cameraYaw_) * speed;
        cameraPos_.z += cosf(cameraYaw_) * speed;
    }
    if (EmscriptenGetKeyState(65)) { // A
        cameraPos_.x -= cosf(cameraYaw_) * speed;
        cameraPos_.z += sinf(cameraYaw_) * speed;
    }
    if (EmscriptenGetKeyState(68)) { // D
        cameraPos_.x += cosf(cameraYaw_) * speed;
        cameraPos_.z -= sinf(cameraYaw_) * speed;
    }
    if (EmscriptenGetKeyState(81)) cameraPos_.y -= speed;  // Q
    if (EmscriptenGetKeyState(69)) cameraPos_.y += speed;  // E

    // Arrow keys: Left=37, Up=38, Right=39, Down=40
    if (EmscriptenGetKeyState(37)) cameraYaw_ += 2.0f * dt;
    if (EmscriptenGetKeyState(39)) cameraYaw_ -= 2.0f * dt;
    if (EmscriptenGetKeyState(38)) cameraPitch_ += 2.0f * dt;
    if (EmscriptenGetKeyState(40)) cameraPitch_ -= 2.0f * dt;

    // Mouse drag for camera rotation
    float mdx, mdy;
    EmscriptenGetMouseDelta(&mdx, &mdy);
    if (EmscriptenGetMouseButton(0)) { // left button drag
        cameraYaw_ -= mdx * 0.003f;
        cameraPitch_ -= mdy * 0.003f;
    }
#endif

    UpdateCameraView();
}

void Engine_Test::UpdateCameraView() {
    namespace rhimath = primal::graphics::rhi::math;

    // Build forward direction from yaw/pitch
    float cosPitch = cosf(cameraPitch_);
    primal::math::v3 forward{
        -sinf(cameraYaw_) * cosPitch,
        sinf(cameraPitch_),
        -cosf(cameraYaw_) * cosPitch
    };

    primal::math::v3 target = cameraPos_ + forward;
    primal::math::v3 up{0.0f, 1.0f, 0.0f};

    rhimath::m4x4 viewMat = rhimath::CreateLookAtMatrix(cameraPos_, target, up);
    view_.SetViewMatrix(viewMat);
    view_.Cull(scene_);
}

void Engine_Test::CreateDepthTexture() {
    if (depthTexture_ != rhi::handles::INVALID_RESOURCE) return;
    rhi::TextureDesc depthDesc;
    depthDesc.size = {width_, height_, 1};
    depthDesc.format = rhi::DataFormat::D32_Float;
    depthDesc.type = rhi::TextureType::Texture2D;
    depthDesc.mipLevels = 1;
    depthDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    depthTexture_ = device_->CreateTexture(depthDesc);
}

void Engine_Test::DestroyDepthTexture() {
    if (depthTexture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(depthTexture_);
        depthTexture_ = rhi::handles::INVALID_RESOURCE;
    }
}

void Engine_Test::CreateShadowResources() {
    // Shadow depth texture (2048x2048 D32_Float)
    rhi::TextureDesc shadowDesc;
    shadowDesc.size = {2048, 2048, 1};
    shadowDesc.format = rhi::DataFormat::D32_Float;
    shadowDesc.type = rhi::TextureType::Texture2D;
    shadowDesc.mipLevels = 1;
    shadowDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    shadowDepthTexture_ = device_->CreateTexture(shadowDesc);

    // Shadow depth pipeline layout: 1 group with uniform buffer (ShadowPerObject)
    {
        rhi::DescriptorSetLayoutBinding bind{};
        bind.binding = 0;
        bind.descriptorType = rhi::DescriptorType::UniformBuffer;
        bind.descriptorCount = 1;
        bind.stageFlags = rhi::ShaderStage::Vertex;
        rhi::DescriptorSetLayoutDesc dslDesc;
        dslDesc.bindingCount = 1;
        dslDesc.bindings = &bind;
        shadowDSL_ = device_->CreateDescriptorSetLayout(dslDesc);

        rhi::PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &shadowDSL_;
        shadowPipelineLayout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Shadow depth pipeline (vertex-only, no pixel shader)
    {
        auto platform = device_->GetPlatform();
        std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "ShadowDepth");
        std::string src = LoadShaderSource(shaderPath);
        if (src.empty()) {
            std::cerr << "[Shadow] Failed to load ShadowDepth.wgsl" << std::endl;
            return;
        }
        auto vs = device_->CreateShader(src.data(), src.size(), rhi::ShaderStage::Vertex, "shadow_vs");
        if (vs == rhi::handles::INVALID_SHADER) {
            std::cerr << "[Shadow] Failed to compile ShadowDepth vertex shader" << std::endl;
            return;
        }

        rhi::GraphicsPipelineDesc pipeDesc;
        pipeDesc.vertexShader = vs;
        pipeDesc.layout = shadowPipelineLayout_;
        pipeDesc.topology = rhi::PrimitiveTopology::TriangleList;
        pipeDesc.renderTargetCount = 0; // depth-only
        pipeDesc.depthStencilFormat = rhi::DataFormat::D32_Float;
        pipeDesc.enableDepthTest = true;
        pipeDesc.enableDepthWrite = true;
        pipeDesc.depthFunc = rhi::ComparisonFunc::Less;
        pipeDesc.cullMode = rhi::CullMode::Front; // front-face culling reduces shadow acne
        // Same 32-byte interleaved vertex layout as ForwardPBR
        utl::vector<rhi::VertexInputAttribute> shadowAttrs(5);
        shadowAttrs[0] = {0, 0, rhi::DataFormat::RGB32_Float, 0};
        shadowAttrs[1] = {1, 0, rhi::DataFormat::R32_UInt, 12};
        shadowAttrs[2] = {2, 0, rhi::DataFormat::R32_UInt, 16};
        shadowAttrs[3] = {3, 0, rhi::DataFormat::R32_UInt, 20};
        shadowAttrs[4] = {4, 0, rhi::DataFormat::RG32_Float, 24};
        pipeDesc.vertexAttributes = shadowAttrs;
        utl::vector<rhi::VertexInputBinding> shadowBindings(1);
        shadowBindings[0] = {0, 32, true};
        pipeDesc.vertexBindings = shadowBindings;
        shadowPipeline_ = device_->CreateGraphicsPipeline(pipeDesc);
        if (shadowPipeline_ == rhi::handles::INVALID_PIPELINE) {
            std::cerr << "[TestDawnFR] Shadow pipeline creation FAILED" << std::endl;
        }
    }

    // Shadow sampler (clamp, border white)
    {
        rhi::SamplerDesc sDesc;
        sDesc.minFilter = rhi::FilterMode::Linear;
        sDesc.magFilter = rhi::FilterMode::Linear;
        sDesc.addressU = rhi::TextureAddressMode::Clamp;
        sDesc.addressV = rhi::TextureAddressMode::Clamp;
        sDesc.addressW = rhi::TextureAddressMode::Clamp;
        sDesc.comparisonFunc = rhi::ComparisonFunc::Never;
        shadowSampler_ = device_->CreateSampler(sDesc);
    }

    // Per-object buffer for shadow pass (128 bytes per entry, mapped)
    {
        rhi::BufferDesc bufDesc{};
        bufDesc.size = 128;
        bufDesc.type = rhi::BufferType::Constant;
        bufDesc.usage = rhi::GPUMemoryUsage::Dynamic;
        bufDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        shadowPerObjectBuf_ = device_->CreateBuffer(bufDesc);
        shadowPerObjectMapped_ = device_->MapBuffer(shadowPerObjectBuf_, 0, 128);

        // Persistent descriptor set for shadow per-object buffer
        rhi::DescriptorSetDesc dsDesc;
        dsDesc.layout = shadowDSL_;
        shadowPerObjectSet_ = device_->CreateDescriptorSet(dsDesc);
        rhi::DescriptorBufferInfo bufInfo;
        bufInfo.buffer = shadowPerObjectBuf_;
        bufInfo.offset = 0;
        bufInfo.range = 128;
        rhi::WriteDescriptorSet write;
        write.dstSet = shadowPerObjectSet_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = rhi::DescriptorType::UniformBuffer;
        write.bufferInfo = &bufInfo;
        device_->UpdateDescriptorSets(1, &write);
    }

    // Pass shadow resources to ForwardRenderer (bindings 13, 14 in Group 0)
    forwardRenderer_.SetDawnShadowResources(shadowDepthTexture_, shadowSampler_);

    std::cout << "[TestDawnFR] Shadow resources: pipeline=" << shadowPipeline_
              << " depthTex=" << shadowDepthTexture_
              << " sampler=" << shadowSampler_ << std::endl;
}

primal::math::m4x4 Engine_Test::ComputeLightViewProjection() const {
    // Directional light direction (same as scene setup)
    primal::math::v3 lightDir = primal::math::v3{0.5f, -0.7f, 0.3f};
    float len = sqrtf(lightDir.x * lightDir.x + lightDir.y * lightDir.y + lightDir.z * lightDir.z);
    lightDir = lightDir / len;

    // Light looks from above toward scene center
    primal::math::v3 lightTarget = cameraPos_;
    primal::math::v3 lightEye = lightTarget - lightDir * 30.0f;
    primal::math::v3 up{0.0f, 1.0f, 0.0f};

    rhimath::m4x4 lightView = rhimath::CreateLookAtMatrix(lightEye, lightTarget, up);

    // Orthographic projection covering the visible scene area
    float orthoSize = 25.0f;
    rhimath::m4x4 lightProj = rhimath::CreateOrthographicMatrix(
        -orthoSize, orthoSize, -orthoSize, orthoSize, 0.1f, 80.0f);

    return lightProj * lightView;
}

void Engine_Test::RenderShadowPass(rhi::RHICommandBuffer* cmd) {
    if (shadowPipeline_ == rhi::handles::INVALID_PIPELINE) return;
    if (shadowDepthTexture_ == rhi::handles::INVALID_RESOURCE) return;
    if (shadowDepthTexture_ == rhi::handles::INVALID_RESOURCE) return;

    lightVP_ = ComputeLightViewProjection();

    // Begin depth-only render pass
    rhi::RenderPassDesc passDesc;
    passDesc.depthAttachment.texture = shadowDepthTexture_;
    passDesc.depthAttachment.format = rhi::DataFormat::D32_Float;
    passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
    passDesc.depthAttachment.clearValue.depth = 1.0f;
    passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;

    rhi::ViewportDesc vp;
    vp.size = {2048.0f, 2048.0f};
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;

    cmd->BeginRenderPass(passDesc);
    cmd->SetViewport(vp);
    cmd->SetScissor({{0, 0}, {2048, 2048}});
    cmd->BindGraphicsPipeline(shadowPipeline_);

    // Bind persistent shadow per-object descriptor set
    if (shadowPerObjectSet_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, shadowPipelineLayout_, 0, 1, &shadowPerObjectSet_, 0, nullptr);
    }

    // Render each mesh
    u32 meshCount = 0;
    for (u32 i = 0; i < sceneMeshInfos_.size(); ++i) {
        auto& meshInfo = sceneMeshInfos_[i];
        if (!meshInfo.mesh || !meshInfo.mesh->IsValid()) continue;

        // Upload per-object data: world (identity) + worldLightVP (lightVP * world)
        if (shadowPerObjectMapped_) {
            meshCount++;
            auto* p = static_cast<float*>(shadowPerObjectMapped_);
            primal::math::m4x4 identity = rhimath::MatrixIdentity();
            memcpy(p, &identity, 64);
            primal::math::m4x4 worldLightVP = lightVP_;
            memcpy(p + 16, &worldLightVP, 64);
            device_->SetBufferDirtySize(shadowPerObjectBuf_, 128);
        }

        meshInfo.mesh->Draw(cmd);
    }

    cmd->EndRenderPass();

    // Barrier: depth texture → ShaderResource for forward pass sampling
    {
        rhi::ResourceBarrier b{};
        b.resource = shadowDepthTexture_;
        b.beforeState = rhi::ResourceState::DepthStencil;
        b.afterState = rhi::ResourceState::ShaderResource;
        b.subresource = 0xFFFFFFFF;
        cmd->InsertBarrier(&b, 1);
    }
}

#ifdef __APPLE__
void Engine_Test::DisplayLinkCallback(CFRunLoopTimerRef, void* info) {
    auto* test = static_cast<Engine_Test*>(info);
    test->RenderFrame();
}
#endif

void Engine_Test::run() {
    std::cout << "[TestDawnForwardRenderer] Running interactive mode (WASD to move, Q/E up/down, ESC to quit)..." << std::endl;
    timer_.begin();
    lastFrameTime_ = std::chrono::steady_clock::now();
}

void Engine_Test::shutdown() {
    forwardRenderer_.Shutdown();
    DestroyDepthTexture();

    // Shadow resources
    if (shadowDepthTexture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(shadowDepthTexture_);
        shadowDepthTexture_ = rhi::handles::INVALID_RESOURCE;
    }
    if (shadowPipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(shadowPipeline_);
        shadowPipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    if (shadowPipelineLayout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(shadowPipelineLayout_);
        shadowPipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }
    if (shadowDSL_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyDescriptorSetLayout(shadowDSL_);
        shadowDSL_ = rhi::handles::INVALID_RESOURCE;
    }
    if (shadowSampler_ != rhi::handles::INVALID_SAMPLER) {
        device_->DestroySampler(shadowSampler_);
        shadowSampler_ = rhi::handles::INVALID_SAMPLER;
    }
    if (shadowPerObjectBuf_ != rhi::handles::INVALID_RESOURCE) {
        if (shadowPerObjectMapped_) device_->UnmapBuffer(shadowPerObjectBuf_);
        device_->DestroyBuffer(shadowPerObjectBuf_);
        shadowPerObjectBuf_ = rhi::handles::INVALID_RESOURCE;
        shadowPerObjectMapped_ = nullptr;
    }
    if (shadowPerObjectSet_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        device_->DestroyDescriptorSet(shadowPerObjectSet_);
        shadowPerObjectSet_ = rhi::handles::INVALID_DESCRIPTOR_SET;
    }

    if (hdrTexture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(hdrTexture_);
        hdrTexture_ = rhi::handles::INVALID_RESOURCE;
    }
    renderGraph_.reset();

    // Destroy material instances and meshes
    materials_.clear();
    for (auto& info : sceneMeshInfos_) {
        if (info.mesh) {
            info.mesh->Destroy(device_);
        }
    }
    sceneMeshInfos_.clear();

    if (pipelineLayout_ != INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(pipelineLayout_);
        pipelineLayout_ = INVALID_PIPELINE_LAYOUT;
    }
    if (materialSetLayout_ != INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(materialSetLayout_);
        materialSetLayout_ = INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    if (materialSampler_ != INVALID_RESOURCE) {
        device_->DestroySampler(materialSampler_);
        materialSampler_ = INVALID_RESOURCE;
    }

    if (swapchain_) {
        device_->DestroySwapChain(swapchain_);
        swapchain_ = nullptr;
    }

    if (cmdBuffer_ != rhi::handles::INVALID_COMMAND_BUFFER) {
        device_->DestroyCommandBuffer(cmdBuffer_);
        cmdBuffer_ = rhi::handles::INVALID_COMMAND_BUFFER;
    }
    if (postCmdBuffer_ != rhi::handles::INVALID_COMMAND_BUFFER) {
        device_->DestroyCommandBuffer(postCmdBuffer_);
        postCmdBuffer_ = rhi::handles::INVALID_COMMAND_BUFFER;
    }

    graphics::PostProcess::ShutdownBloomPass();
    graphics::PostProcess::ShutdownSSAOPass();
    graphics::PostProcess::ShutdownLumenSSGIPass();

    if (device_) {
        device_->Shutdown();
        delete device_;
        device_ = nullptr;
    }

    if (window_.is_valid()) {
        platform::remove_window(window_.get_id());
    }
}

#ifdef __APPLE__
void Engine_Test::applicationDidFinishLaunching(NS::Notification* notification) {
    std::cerr << "[TestDawnForwardRenderer] applicationDidFinishLaunching called" << std::endl;
    NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
    pApp->activateIgnoringOtherApps(true);

    if (!initialize()) {
        std::cerr << "[TestDawnForwardRenderer] Initialization FAILED" << std::endl;
        NS::Application::sharedApplication()->terminate(nullptr);
        return;
    }

    run();

    // Set up display link on the main run loop (same pattern as RenderTestRunner).
    // The main run loop is managed by app->run(), so we don't nest another CFRunLoopRun().
    runLoop_ = CFRunLoopGetMain();
    CFRunLoopTimerContext ctx{};
    ctx.info = this;
    displayLink_ = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 1.0/60.0,
        1.0/60.0, 0, 0, DisplayLinkCallback, &ctx);
    CFRunLoopAddTimer(runLoop_, displayLink_, kCFRunLoopCommonModes);
}

void Engine_Test::applicationWillFinishLaunching(NS::Notification* notification) {
    NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
    pApp->setActivationPolicy(NS::ActivationPolicy::ActivationPolicyRegular);
}

bool Engine_Test::applicationShouldTerminateAfterLastWindowClosed(NS::Application*) {
    shuttingDown_ = true;

    // Clean up display link before shutdown
    if (displayLink_) {
        CFRunLoopRemoveTimer(runLoop_, displayLink_, kCFRunLoopCommonModes);
        CFRelease(displayLink_);
        displayLink_ = nullptr;
    }

    shutdown();

    timer_.end();
    std::cout << "[TestDawnForwardRenderer] " << totalFrames_ << " frames done (avg "
              << timer_.dt_avg() * 1000.0f << " ms/frame)" << std::endl;

    // Use _exit(0) to terminate immediately after cleanup.
    // exit()/terminate: runs static destructors (free_list assertion crash).
    // Returning to app->run() triggers CA::Transaction commit on destroyed
    // Metal textures (image_finalize crash). _exit skips both.
    _exit(0);
}
#endif

#endif // ENABLE_WEBGPU
