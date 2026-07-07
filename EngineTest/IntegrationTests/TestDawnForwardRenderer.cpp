#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
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
#include "Engine/Graphics/Dawn/ShaderLoader.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/SSAOPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/HZBPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/VelocityPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/TAAPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/SSRPass.h"
#include "Engine/Graphics/RenderPipeline/RenderPasses/PostProcess/LumenSSGIDawnPass.h"
#include "Engine/Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Engine/Graphics/Nanite/GPUCullingPipeline.h"
#include "Engine/Graphics/Nanite/HZBSystem.h"
#include "Engine/Graphics/Nanite/GPUMaterialRegistry.h"
#include "Engine/Graphics/Nanite/NaniteResourceManager.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RHI/Core/RHIMeshAsset.h"
#include "Engine/Graphics/Lumen/StaticProbe/StaticProbeBaker.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Cluster.h"
#include "Engine/JobSystem/JobSystem.h"
#include "Engine/Graphics/RenderProxy.h"
#include "Engine/Graphics/Material.h"
#include "Engine/Graphics/MaterialInstance.h"
#include "Engine/Graphics/RenderMesh.h"
#include "Engine/Graphics/RHI/Utils/ShadowUtils.h"
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>

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
    // Include CopySource so meshlet path can blit textures into the material
    // texture arrays (AlbedoTextureArray / NormalTextureArray / ORMTextureArray).
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | TextureUsage::CopySource;

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

// Float32 → Float16 conversion for HDR data
static uint16_t f32_to_f16(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    uint16_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = ((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = (bits >> 13) & 0x3FFu;
    if (exp <= 0) return sign;
    if (exp >= 31) return sign | 0x7C00u;
    return sign | (uint16_t(exp) << 10) | uint16_t(mantissa);
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
    std::cout << "[TestDawnFR] Init..." << std::endl;

    // Initialize JobSystem early — GPUMaterialRegistry::BuildAsync schedules on it.
    if (!primal::jobsystem::JobSystem::Initialize(primal::jobsystem::JobSchedulerConfig::Default())) {
        std::cerr << "[TestDawnFR] JobSystem init failed" << std::endl;
        return false;
    }

    // Create window
    platform::window_init_info windowInfo{
        nullptr, nullptr,
        "Dawn Forward Renderer | [Tab] Switch Mode | Mode 2: ShadowAndIBL",
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
        std::cerr << "[TestDawnFR] Dawn device failed" << std::endl;
        return false;
    }

    // Register with global device manager so content::create_resource can
    // reach the Dawn device when GPUMaterialRegistry creates placeholder
    // textures. Without this, create_texture_resource falls through to the
    // legacy add_texture path which doesn't support WebGPU.
    rhi::g_deviceManager.RegisterDevice(device_);

    // Create persistent depth texture (reused across frames)
    CreateDepthTexture();
    CreatePrepassDepthTexture();

    // Create persistent HDR render target for post-processing
    hdrDesc_.size = {width_, height_, 1};
    hdrDesc_.format = rhi::DataFormat::RGBA16_Float;
    hdrDesc_.type = rhi::TextureType::Texture2D;
    hdrDesc_.mipLevels = 1;
    hdrDesc_.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource |
                     rhi::TextureUsage::CopyDest | rhi::TextureUsage::UnorderedAccess;
    hdrTexture_ = device_->CreateTexture(hdrDesc_);

    // Create persistent Velocity MRT texture (RG16F, written by ForwardPBR fragment)
    rhi::TextureDesc velDesc{};
    velDesc.size = {width_, height_, 1};
    velDesc.format = rhi::DataFormat::RG16_Float;
    velDesc.type = rhi::TextureType::Texture2D;
    velDesc.mipLevels = 1;
    // Meshlet path blits GBuffer velocity → velocityTexture_; the source needs CopySrc.
    velDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource |
                    rhi::TextureUsage::CopySource | rhi::TextureUsage::CopyDest;
    velocityTexture_ = device_->CreateTexture(velDesc);

    // Create G-Buffer textures for Deferred mode (Phase 3b)
    // RT0: WorldPos RGBA16F, RT1: Normal RGBA16F, RT2: Albedo RGBA8_sRGB,
    // RT3: ORM RGBA8, RT4: Velocity RG16F (mirrors velocityTexture_ format)
    {
        rhi::DataFormat gbufferFormats[kGBufferRTCount] = {
            rhi::DataFormat::RGBA16_Float,
            rhi::DataFormat::RGBA16_Float,
            rhi::DataFormat::RGBA8_sRGB,
            rhi::DataFormat::RGBA8_UNorm,
            rhi::DataFormat::RG16_Float
        };
        for (u32 i = 0; i < kGBufferRTCount; ++i) {
            rhi::TextureDesc gbDesc{};
            gbDesc.size = {width_, height_, 1};
            gbDesc.format = gbufferFormats[i];
            gbDesc.type = rhi::TextureType::Texture2D;
            gbDesc.mipLevels = 1;
            // CopySource: needed for the 3b-1 visualization blit (RT0 → hdrTexture_).
            // 3c deferred lighting will sample via ShaderResource instead.
            gbDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopySource;
            gbufferTextures_[i] = device_->CreateTexture(gbDesc);
        }
    }

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
        std::cerr << "[TestDawnFR] Swapchain failed" << std::endl;
        return false;
    }

    // Initialize ForwardRenderer
    if (!forwardRenderer_.Initialize(device_)) {
        std::cerr << "[TestDawnFR] ForwardRenderer failed" << std::endl;
        return false;
    }

    CreateShadowResources();

    // Create IBL resources (loads HDR env map + runs compute precomputation)
    CreateIBLResources();

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

    // Phase N2: lazy-init meshlet pipeline (mode 7/8). Init is cheap if mode 7
    // is never selected — just creates singleton handles + GPU buffers.
    InitializeMeshletPipeline();

    // DEBUG: env var override for non-interactive mode testing.
    if (const char* modeEnv = std::getenv("DAWN_FORCE_MODE")) {
        int m = std::atoi(modeEnv);
        if (m >= 0 && m < static_cast<int>(DawnRenderMode::Count)) {
            renderMode_ = static_cast<DawnRenderMode>(m);
            std::cerr << "[TestDawnFR] DAWN_FORCE_MODE=" << m
                      << " forcing mode on startup" << std::endl;
        }
    }

    std::cout << "[TestDawnFR] Ready: " << sceneMeshInfos_.size()
              << " meshes, " << view_.GetVisibleProxies().size() << " visible" << std::endl;

#ifdef __EMSCRIPTEN__
    g_engineTest = this;
    // Create HUD dynamically (independent of shell.html)
    EM_ASM({
        if (!document.getElementById('modeHud')) {
            var hud = document.createElement('div');
            hud.id = 'modeHud';
            hud.style.cssText = 'position:fixed;top:12px;left:12px;background:rgba(0,0,0,0.85);color:#fff;font-size:13px;padding:10px 16px;border-radius:8px;line-height:1.6;z-index:9999;font-family:monospace;border:1px solid #333;';
            hud.innerHTML = '<div style="font-weight:bold;color:#00d4ff;margin-bottom:4px;">Dawn Forward Renderer</div>'
                + '<div>Press <kbd style="background:#333;padding:1px 6px;border-radius:3px;">Tab</kbd> to switch render mode</div>'
                + '<div>Press <kbd style="background:#333;padding:1px 6px;border-radius:3px;">V</kbd> to cycle meshlet debug (mode 7/8)</div>'
                + '<div id="modeHudCurrent" style="margin-top:4px;color:#4f4;">Mode 2: ShadowAndIBL</div>'
                + '<div id="modeHudDesc" style="color:#aaa;">Directional + Shadow + IBL</div>'
                + '<div id="meshletDbgHud" style="color:#fd0;display:none;">Meshlet Debug: Off</div>';
            document.body.appendChild(hud);
        }
        // Define HUD update function (independent of shell.html)
        window.setRenderMode = function(idx, name, desc) {
            var c = document.getElementById('modeHudCurrent');
            var d = document.getElementById('modeHudDesc');
            if (c) c.textContent = 'Mode ' + idx + ': ' + name;
            if (d) d.textContent = desc;
            // Show the meshlet-debug status line only in modes 7/8.
            var dbg = document.getElementById('meshletDbgHud');
            if (dbg) dbg.style.display = (idx == 7 || idx == 8) ? 'block' : 'none';
        };
        // Update meshlet debug label (called from C++ on each V press).
        // Avoid array-literal commas in EM_ASM — the C preprocessor treats top-
        // level commas as macro-argument separators.
        window.setMeshletDebug = function(mode) {
            var dbg = document.getElementById('meshletDbgHud');
            if (!dbg) return;
            var label = 'Off';
            if (mode == 1) label = 'MeshletID';
            else if (mode == 2) label = 'TriangleID';
            else if (mode == 3) label = 'MeshID';
            else if (mode == 4) label = 'Normal';
            else if (mode == 5) label = 'ObjNormal';
            dbg.textContent = 'Meshlet Debug: ' + label;
        };
    });
#endif

    return true;
}

bool Engine_Test::LoadSponzaScene() {
#ifdef __EMSCRIPTEN__
    std::string baseDir = "assets/";
#else
    std::string baseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
#endif
    // Prefer the processed rebuild — it includes pre-built meshlet clusters
    // required by Mode 7-10 (meshlet pipeline). The bare Sponza.model lacks
    // MSHL data and forces cluster synthesis, which is incomplete (see
    // dawn-meshlet-cluster-count-synthesis-bug) and produces magenta output
    // in Mode 10 (empty meshlet GBuffer → deferred lighting reads garbage).
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

    // Load Dawn-compatible PBR shader with shadow sampling
#ifdef __EMSCRIPTEN__
    std::string shaderSource = dawn::LoadWGSL("ForwardPBR");
#else
    std::string shaderSource = LoadShaderSource("Engine/Graphics/Dawn/shaders/ForwardPBR.wgsl");
#endif
    if (shaderSource.empty()) {
        std::cerr << "[TestDawnFR] Failed to load ForwardPBR.wgsl" << std::endl;
        return false;
    }
    std::cerr << "[TestDawnFR] Shader loaded, size=" << shaderSource.size() << std::endl;

    // Create shared Material for all meshes
    auto material = std::make_shared<Material>();
    // Include null terminator for WGSL source text
    material->SetShader(ShaderStage::Vertex, shaderSource.data(), shaderSource.size() + 1, "vertexMain");
    material->SetShader(ShaderStage::Pixel, shaderSource.data(), shaderSource.size() + 1, "fragmentMain");
    std::cerr << "[TestDawnFR] Material shaders set" << std::endl;

    // Vertex attributes (32-byte interleaved: pos + colorTSign + packedNormal + packedTangent + uv)
    primal::utl::vector<VertexInputAttribute> attrs(5);
    attrs[0] = {0, 0, DataFormat::RGB32_Float, 0};   // position
    attrs[1] = {1, 0, DataFormat::R32_UInt, 12};      // colorTSign
    attrs[2] = {2, 0, DataFormat::R32_UInt, 16};      // packedNormal
    attrs[3] = {3, 0, DataFormat::R32_UInt, 20};      // packedTangent
    attrs[4] = {4, 0, DataFormat::RG32_Float, 24};    // uv
    material->SetVertexAttributes(attrs);

    primal::utl::vector<VertexInputBinding> bindings(1);
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

    // Render target format — HDR + velocity MRT for Phase 2 TAA/SSR
    primal::utl::vector<DataFormat> rtFormats(2);
    rtFormats[0] = DataFormat::RGBA16_Float; // Render to HDR texture
    rtFormats[1] = DataFormat::RG16_Float;   // Velocity motion vectors
    material->SetRenderTargetFormats(rtFormats, DataFormat::D32_Float);

    // Pipeline layout: 3 descriptor set layouts matching ForwardPBR.wgsl
    // Group 0 (Global Dawn): binding 11 = GlobalShaderData(UB), binding 12 = ForwardLightBuffer(UB)
    //                         binding 13 = shadowDepthTex, binding 14 = shadowSampler
    // Group 1 (PerObject): binding 10 = PerObjectData(DynamicUB)
    // Group 2 (Material): binding 0 = albedo, 1 = normal, 2 = ORM, 3 = sampler

    // Use ForwardRenderer's Dawn-compatible global layout (4 bindings: 11,12,13,14)
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

    // Phase 3c-2: hand the material DSL to ForwardRenderer so its lazily-created
    // G-Buffer pipeline can include group 1 (material textures) in the layout.
    forwardRenderer_.SetDawnGBufferMaterialDSL(materialSetLayout_);

    // Pipeline layout: 3 groups (Global with shadow bindings, PerObject, Material)
    // Create per-object layout fresh (avoid handle corruption from IBL heap operations)
    DescriptorSetLayoutBinding perObjectBinding{};
    perObjectBinding.binding = 0;
    perObjectBinding.descriptorType = DescriptorType::UniformBufferDynamic;
    perObjectBinding.descriptorCount = 1;
    perObjectBinding.stageFlags = ShaderStage::Vertex;
    DescriptorSetLayoutDesc perObjLayoutDesc{1, &perObjectBinding};
    auto perObjectLayout = device_->CreateDescriptorSetLayout(perObjLayoutDesc);
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
    std::cerr << "[TestDawnFR] Pipeline layout created" << std::endl;

    // Pre-warm the ForwardPBR pipeline BEFORE any render pass. On WASM/Dawn,
    // calling wgpuDeviceCreateRenderPipeline inside an active render pass
    // encoder produces a pipeline that silently misbinds material descriptor
    // sets — every mesh ends up sampling the same texture. Warming the cache
    // here means OpaquePass's GetPipeline call is a cache hit, not a create.
    {
        auto warmupHandle = material->GetPipeline(device_, INVALID_RESOURCE, 0, PipelineFlags::None);
        if (warmupHandle != INVALID_PIPELINE) {
            std::cerr << "[TestDawnFR] Pipeline pre-warmed, handle=" << warmupHandle << std::endl;
        } else {
            std::cerr << "[TestDawnFR][WARN] Pipeline warm-up failed" << std::endl;
        }
    }

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
    std::cerr << "[TestDawnFR] Creating material instances..." << std::endl;

    // Shared 1024x1024 fallback textures for meshes with missing texture paths.
    // The meshlet path blits sources into texture arrays — 1x1 fallbacks would
    // force the array's reference size to 1x1 and skip every real 1024x1024
    // source. Using 1024x1024 fallbacks keeps array dimensions sane.
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
    ResourceHandle fallbackDiffuse = CreateTextureFromData(
        device_, FALLBACK_SIZE, FALLBACK_SIZE, whiteBuf.data(), DataFormat::RGBA8_sRGB);
    ResourceHandle fallbackNormal = CreateTextureFromData(
        device_, FALLBACK_SIZE, FALLBACK_SIZE, flatNormalBuf.data());
    ResourceHandle fallbackORM = CreateTextureFromData(
        device_, FALLBACK_SIZE, FALLBACK_SIZE, defaultORMBuf.data());

    for (u32 i = 0; i < sceneMeshInfos_.size(); ++i) {
        auto& meshInfo = sceneMeshInfos_[i];
        meshInfo.material = material; // Shared material

        if (i < 3 || i == sceneMeshInfos_.size() - 1) {
            std::cerr << "[TestDawnFR] Mesh " << i << "/" << sceneMeshInfos_.size() << std::endl;
        }

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
            diffuseTex = fallbackDiffuse;
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
            normalTex = fallbackNormal;
        }

        // Load ORM texture
        ResourceHandle ormTex = INVALID_RESOURCE;
        std::string ormPath = ResolveTexturePath(textureBase, meshInfo.ormTexturePath);
        if (!ormPath.empty()) {
            ormTex = LoadTextureFromFile(device_, ormPath);
        }
        if (ormTex == INVALID_RESOURCE) {
            ormTex = fallbackORM;
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
        // NOTE: proxy.meshId doubles as the cluster::component lookup key.
        // cluster::create stores the cluster keyed by entity_id (see
        // Cluster.cpp:70-71 — `component c{ entity.get_id() }`). So we set
        // proxy.meshId = entity_id of the entity that owns the cluster.
        // The forward path uses meshInfo.mesh directly (not proxy.meshId), so
        // this is safe.
        proxy.materialId = meshInfo.meshEntityId; // Use entity ID as material key
        proxy.entityId = meshInfo.meshEntityId;

        // Identity transform for Sponza
        // Identity transform
        proxy.transform = rhimath::MatrixIdentity();

        // Compute AABB from mesh if available
        if (meshInfo.mesh && meshInfo.mesh->IsValid()) {
            proxy.worldAABB = meshInfo.mesh->GetLocalAABB();
        }

        // Create Cluster component so RenderSceneSnapshot::ExtractSceneData can
        // resolve geometry_content_id for the meshlet pipeline. Without this,
        // cluster::get(proxy.meshId) returns nullptr and every proxy is skipped.
        if (meshInfo.meshEntityId != primal::id::invalid_id) {
            primal::game_entity::entity_info entInfo{};
            primal::transform::init_info tfInfo{};
            tfInfo.position[0] = 0.0f;
            tfInfo.position[1] = 0.0f;
            tfInfo.position[2] = 0.0f;
            // rotation defaults to {0,0,0,0} which is NOT a valid quaternion —
            // simd_quaternion(0,0,0,0) produces NaN-rotated world_matrix columns.
            // Must be identity {0,0,0,1}. See tasks/lessons.md 2026-06-17.
            tfInfo.rotation[0] = 0.0f;
            tfInfo.rotation[1] = 0.0f;
            tfInfo.rotation[2] = 0.0f;
            tfInfo.rotation[3] = 1.0f;
            entInfo.transform = &tfInfo;
            primal::game_entity::entity entity = primal::game_entity::create(entInfo);
            if (entity.is_valid()) {
                primal::cluster::init_info clusterInit{};
                clusterInit.geometry_content_id = meshInfo.meshEntityId;
                primal::cluster::component clusterComp =
                    primal::cluster::create(clusterInit, entity);
                // cluster::component == entity_id — set proxy.meshId so
                // RenderSceneSnapshot can resolve the cluster.
                proxy.meshId = clusterComp;
            } else {
                proxy.meshId = meshInfo.meshEntityId;
            }
        } else {
            proxy.meshId = meshInfo.meshEntityId;
        }

        scene_.AddProxy(proxy);

        // Add to materials map (keyed by materialId = entityId)
        materials_[proxy.materialId] = matInst;
    }

    std::cerr << "[TestDawnFR] Material instances created: " << texLoaded << " loaded, " << texFailed << " failed" << std::endl;

    return true;
}

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

    // ============================================================
    // Full render path: Shadow → Forward(HDR) → Post-processing → Present
    // ============================================================

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
    // Downstream passes (HZB/SSR/SSAO) consume the NON-jITTERED prepass depth, not the
    // jittered forward depth — that's what keeps reflections stable while TAA jitters color.
    auto depthRG = renderGraph_->ImportTexture("PrepassDepth", prepassDepthTexture_, depthDescForRG);

    // In meshlet modes (7/8), prepassDepthTexture_ is NEVER written — we skip
    // RenderDawnDepthPrepass because the meshlet pipeline owns depth. Feeding
    // the stale prepass texture to HZB/SSAO produced a static AO overlay that
    // read as a persistent "ghost" over the moving meshlet result. Mirror the
    // Metal test (TestNaniteStreamingPipeline.cpp:3340-3341) and import the
    // meshlet pipeline's own final_depth_texture_ for post-process depth reads.
    // Forward/Deferred paths still use prepassDepthTexture_.
    // `meshletMode` covers every mode that runs RenderMeshletFrame — including
    // Mode 9, which reuses the meshlet draw path. Mode 10 (MeshletSSGISSRDDGI)
    // = Mode 9 + DDGI and also reuses the meshlet path (DDGI is layered on top
    // via the enableDDGI flag + giIndirectTexture_ binding inside
    // RenderMeshletFrame). This flag gates the meshlet depth import (rebinds
    // depthRG from prepassDepthTexture_ to
    // GPUDrivenDrawPipeline::GetFinalDepthTexture()) below.
    const bool meshletMode = (renderMode_ == DawnRenderMode::MeshletNoIBL ||
                              renderMode_ == DawnRenderMode::Meshlet ||
                              renderMode_ == DawnRenderMode::MeshletSSGISSR ||
                              renderMode_ == DawnRenderMode::MeshletSSGISSRDDGI);
    // The TAA-bypass diagnostic is a SEPARATE concern: Mode 7/8 keep the bypass,
    // Mode 9 runs TAA (its SSGI/SSR depend on the resolved HDR).
    const bool meshletBypassTAA = (renderMode_ == DawnRenderMode::MeshletNoIBL ||
                                   renderMode_ == DawnRenderMode::Meshlet);
    if (meshletMode) {
        auto& gpuDrawPipeline = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
        rhi::ResourceHandle meshletDepth = gpuDrawPipeline.GetFinalDepthTexture();
        if (meshletDepth != rhi::handles::INVALID_RESOURCE) {
            depthRG = renderGraph_->ImportTexture("MeshletDepth", meshletDepth, depthDescForRG);
        }
    }

    // Import velocity MRT texture (RG16F) for ToneMapping debug visualization
    rhi::TextureDesc velDescForRG;
    velDescForRG.size = {width_, height_, 1};
    velDescForRG.format = rhi::DataFormat::RG16_Float;
    velDescForRG.type = rhi::TextureType::Texture2D;
    velDescForRG.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    auto velMrtRG = renderGraph_->ImportTexture("VelocityMRT", velocityTexture_, velDescForRG);

    auto invProj = rhimath::Inverse(view_.GetProjectionMatrix());

    // HZB generation from depth buffer — needed for SSAO/SSR consumers.
    // (NoEffects/ShadowOnly skip — they're visualization modes.)
    auto hzbHandle = rendergraph::kInvalidRGResourceHandle;
    if (renderMode_ != DawnRenderMode::NoEffects && renderMode_ != DawnRenderMode::ShadowOnly) {
        const auto& hzbOut = PostProcess::AddHZBPass(*renderGraph_, depthRG, width_, height_);
        hzbHandle = hzbOut.hzbTexture;
    }

    // 3c: Deferred path now goes through the full post-process chain (TAA →
    // SSAO → ToneMap) just like Full/FullPlusSSR. The HDR written by the
    // deferred lighting compute is in the same linear-HDR space the forward
    // path produces, so TAA mixing and SSAO AO multiplication work the same.
    auto taaHDR = hdrRG;
    auto tonemapOutput = hdrRG;

    if (renderMode_ != DawnRenderMode::NoEffects && renderMode_ != DawnRenderMode::ShadowOnly) {
        // Mode 8 (Meshlet) keeps its diagnostic bypass — see the velocity blit
        // comment in RenderMeshletFrame (RG16_Float format reconciliation).
        // Mode 9 (MeshletSSGISSR) runs TAA; SSGI/SSR depend on the resolved HDR.
        // Velocity audit confirmed meshlet RG16F velocity is consistent end-to-end.
        if (!meshletBypassTAA) {
            const auto& taaOut = PostProcess::AddTAAPass(*renderGraph_, hdrRG, velMrtRG, width_, height_, fi);
            taaHDR = taaOut.output;
        }

        // SSR: trace reflection rays (half-res), temporal accumulate, composite into HDR.
        const bool ssrActive =
            renderMode_ == DawnRenderMode::FullPlusSSR ||
            renderMode_ == DawnRenderMode::Deferred ||
            (renderMode_ == DawnRenderMode::MeshletSSGISSR &&
             (ssgissrSubmode_ == SSGISSRSubmode::SSROnly || ssgissrSubmode_ == SSGISSRSubmode::Both));
        if (ssrActive) {
            // Pass frameIndex_ (true counter) — same reason as SSGI: SSR's
            // temporal jitter hashes by params.frameIndex and would otherwise
            // see only 3 distinct seeds cycling.
            const auto& ssrOut = PostProcess::AddSSRPass(*renderGraph_, taaHDR, depthRG, hzbHandle,
                                                          velMrtRG, width_, height_, frameIndex_,
                                                          view_.GetProjectionMatrix(), invProj);
            taaHDR = ssrOut.outputColor;
        }

        // SSGI (Lumen Dawn variant): half-res HZB ray march gathering prev-frame color.
        // Mode 9 only. prevFrameColor = taaHDR (self-feedback, accepts 1-frame latency).
        auto ssgiHandle = rendergraph::kInvalidRGResourceHandle;
        const bool ssgiActive =
            renderMode_ == DawnRenderMode::MeshletSSGISSR &&
            (ssgissrSubmode_ == SSGISSRSubmode::SSGIOnly || ssgissrSubmode_ == SSGISSRSubmode::Both);
        if (ssgiActive) {
            // Pass frameIndex_ (true counter), not fi (swap-chain index 0..2).
            // The SSGI trace hashes ray directions by params.frameIndex; with
            // only 3 distinct values cycling, phi rotation had just 3 states
            // and the per-frame noise repeated every 3 frames — temporal
            // averaging then converged to a stationary striped pattern.
            const auto& ssgiOut = PostProcess::AddLumenSSGIPass(*renderGraph_,
                depthRG, hzbHandle, velMrtRG, taaHDR,
                width_, height_, frameIndex_,
                view_.GetProjectionMatrix(), invProj);
            ssgiHandle = ssgiOut.ssgiOutput;
        }

        const auto& ssaoOut = graphics::PostProcess::AddSSAOPass(*renderGraph_, depthRG, width_, height_, fi, view_.GetProjectionMatrix(), invProj);
        auto ssaoAOHandle = ssaoOut.ssaoOutput;

        // Bloom disabled — BlurPass WGSL/C++ struct mismatch + heap corruption causes validation errors
        auto bloomHandle = rendergraph::kInvalidRGResourceHandle;

        const auto& tonemapOut = PostProcess::AddToneMappingPass(*renderGraph_, taaHDR, bloomHandle,
            ssaoAOHandle, ssgiHandle, velMrtRG, fi);
        tonemapOutput = tonemapOut.output;
    }

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
    // Pre-allocated descriptor set pool for present pass
    static constexpr u32 PRESENT_MAX_FRAMES = 3;
    static constexpr u32 PRESENT_MAX_SETS = 2;
    static rhi::DescriptorSetHandle s_presentSets[PRESENT_MAX_FRAMES][PRESENT_MAX_SETS] = {};
    static u32 s_presentSetIdx[PRESENT_MAX_FRAMES] = {};

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
#ifdef __EMSCRIPTEN__
                std::string src = dawn::LoadWGSL("Blit");
#else
                auto platform = dev.GetPlatform();
                std::string path = utils::ShaderRegistry::GetShaderPath(platform, "Blit");
                std::ifstream f(path);
                std::stringstream buf;
                if (f.is_open()) buf << f.rdbuf();
                std::string src = buf.str();
#endif
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

                // Pre-allocate descriptor set pool for present pass
                for (u32 f = 0; f < PRESENT_MAX_FRAMES; ++f)
                    for (u32 s = 0; s < PRESENT_MAX_SETS; ++s) {
                        rhi::DescriptorSetDesc dsDesc;
                        dsDesc.layout = s_presentDSL;
                        s_presentSets[f][s] = dev.CreateDescriptorSet(dsDesc);
                    }
            }
        },
        [this](const PresentData& data, rendergraph::RenderGraphContext& context) {
            if (s_presentPipeline == rhi::handles::INVALID_PIPELINE) return;

            auto& dev = context.graph->GetDevice();
            auto* inputRes = context.graph->GetResource(data.input);
            auto* outputRes = context.graph->GetResource(data.output);
            if (!inputRes || !outputRes) return;

            rhi::ResourceHandle inputHandle = inputRes->GetPhysicalHandle();

            // Use pre-allocated descriptor set from pool
            u32 presentFi = frameIndex_ % PRESENT_MAX_FRAMES;
            u32 pidx = s_presentSetIdx[presentFi]++;
            if (pidx >= PRESENT_MAX_SETS) { pidx = 0; s_presentSetIdx[presentFi] = 1; }
            rhi::DescriptorSetHandle ds = s_presentSets[presentFi][pidx];

            if (inputHandle != rhi::handles::INVALID_RESOURCE && ds != rhi::handles::INVALID_RESOURCE) {
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
        }
    );

    renderGraph_->Compile();

    // Single submit: Shadow + Forward + Post-processing
    // Dawn's Metal backend crashes when tracking texture sync across separate
    // command buffer submits. Use one command buffer for the entire frame.
    if (cmdBuffer_ == rhi::handles::INVALID_COMMAND_BUFFER) {
        cmdBuffer_ = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    }
    rhi::RHICommandBuffer* cmd = device_->GetCommandBuffer(cmdBuffer_);
    if (cmd) {
        cmd->Reset();
        if (cmd->Begin()) {
            // 1. Shadow pass — skip in NoEffects / Meshlet modes (Meshlet runs
            //    its own shadow pipeline in RenderMeshletFrame).
            if (renderMode_ != DawnRenderMode::NoEffects &&
                renderMode_ != DawnRenderMode::MeshletNoIBL &&
                renderMode_ != DawnRenderMode::Meshlet &&
                renderMode_ != DawnRenderMode::MeshletSSGISSR) {
                RenderShadowPass(cmd);
                forwardRenderer_.SetDawnShadowLightVP(lightVP_);
            }

            // 1b. Non-jittered depth prepass — must run BEFORE Forward so its depth
            //     is already in prepassDepthTexture_ when RG passes (HZB/SSR/SSAO)
            //     execute. Skipped in NoEffects / Meshlet — Meshlet produces its
            //     own depth texture via GPUDrivenDrawPipeline::Execute.
            if (renderMode_ != DawnRenderMode::NoEffects &&
                renderMode_ != DawnRenderMode::MeshletNoIBL &&
                renderMode_ != DawnRenderMode::Meshlet &&
                renderMode_ != DawnRenderMode::MeshletSSGISSR) {
                forwardRenderer_.RenderDawnDepthPrepass(cmd, view_, prepassDepthTexture_, fi, width_, height_);
            }

            // Mode 7/8 (MeshletNoIBL / Meshlet) — runs its own shadow + HZB +
            // cull + draw + meshlet deferred lighting. Produces hdrTexture_ from
            // the 4-RT meshlet GBuffer. Falls through to the renderGraph
            // post-processing pipeline below.
            if (renderMode_ == DawnRenderMode::MeshletSSGISSRDDGI) {
                RenderMeshletDDGIFrame(cmd);
            } else if (renderMode_ == DawnRenderMode::MeshletNoIBL ||
                renderMode_ == DawnRenderMode::Meshlet ||
                renderMode_ == DawnRenderMode::MeshletSSGISSR) {
                RenderMeshletFrame(cmd);
            } else if (renderMode_ == DawnRenderMode::Deferred) {
                // G-Buffer shares the non-jittered prepass depth (LessEqual +
                // no-write). depthTexture_ is only populated by Render(), which
                // is bypassed in Deferred mode — using it would leave the
                // GBuffer reading undefined depth and discarding all geometry.
                forwardRenderer_.RenderDawnGBuffer(cmd, view_, gbufferTextures_, prepassDepthTexture_, materials_, fi, width_, height_);
                forwardRenderer_.RenderDawnDeferredLighting(cmd, view_, gbufferTextures_, hdrTexture_, scene_, fi, width_, height_);

                // Blit RT4 (velocity RG16F) → velocityTexture_ so TAA sees
                // per-pixel motion vectors from the G-Buffer pass.
                rhi::TextureBlitRegion velRegion{};
                velRegion.srcSubresource = {0, 0, 1};
                velRegion.srcOffsets[0] = {0, 0, 0};
                velRegion.srcOffsets[1] = {(s32)width_, (s32)height_, 1};
                velRegion.dstSubresource = {0, 0, 1};
                velRegion.dstOffsets[0] = {0, 0, 0};
                velRegion.dstOffsets[1] = {(s32)width_, (s32)height_, 1};
                cmd->BlitTexture(gbufferTextures_[4], velocityTexture_, &velRegion, 1, rhi::FilterMode::Nearest);
            } else if (renderMode_ == DawnRenderMode::LumenDDGI) {
                // Phase 4 (Lumen DDGI). Plumbing-only stage: behavior currently
                // mirrors Deferred (G-Buffer + Deferred Lighting) so the mode
                // is selectable via Tab. Subsequent 4a/4b/4c/4d steps will
                // insert GlobalSDF dispatch, DDGI trace/update, and the DDGI
                // ambient term in DeferredLighting.
                forwardRenderer_.RenderDawnGBuffer(cmd, view_, gbufferTextures_, prepassDepthTexture_, materials_, fi, width_, height_);
                forwardRenderer_.RenderDawnDeferredLighting(cmd, view_, gbufferTextures_, hdrTexture_, scene_, fi, width_, height_);

                rhi::TextureBlitRegion velRegion{};
                velRegion.srcSubresource = {0, 0, 1};
                velRegion.srcOffsets[0] = {0, 0, 0};
                velRegion.srcOffsets[1] = {(s32)width_, (s32)height_, 1};
                velRegion.dstSubresource = {0, 0, 1};
                velRegion.dstOffsets[0] = {0, 0, 0};
                velRegion.dstOffsets[1] = {(s32)width_, (s32)height_, 1};
                cmd->BlitTexture(gbufferTextures_[4], velocityTexture_, &velRegion, 1, rhi::FilterMode::Nearest);
            } else {
                forwardRenderer_.Render(cmd, scene_, view_, hdrTexture_, velocityTexture_, depthTexture_, materials_, fi, width_, height_);
            }

            // 4. Post-processing (render graph: HZB, SSAO, Bloom, ToneMap, Present)
            renderGraph_->Execute(cmd);

            cmd->End();
        }
        rhi::QueueSubmitInfo submitInfo{};
        submitInfo.cmdBuffer = cmdBuffer_;
        device_->Submit(submitInfo);
    }

    swapchain_->Present(rhi::handles::INVALID_SYNC);
    device_->EndFrame();
    frameIndex_++;
    totalFrames_++;
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

    // Tab (keyCode 48) to cycle render mode — edge detected.
    // V (keyCode 9) cycles meshlet debug visualization (mode 7/8 only).
    {
        static const char* kModeNames[] = {"NoEffects", "ShadowOnly", "ShadowAndIBL", "ShadowAndIBLAndPuncLight", "ShadowAndIBLAndPuncLightAndSSR", "Deferred", "LumenDDGI", "MeshletNoIBL", "Meshlet", "MeshletSSGISSR", "MeshletSSGISSRDDGI"};
        static const char* kModeDesc[] = {
            "Directional light only",
            "Directional + Shadow",
            "Directional + Shadow + IBL",
            "Directional + Shadow + IBL + Punctual",
            "Full + Screen-Space Reflections",
            "G-Buffer + Deferred Lighting",
            "G-Buffer + DDGI Global Illumination",
            "Meshlet pipeline — IBL OFF (A/B vs Mode 8)",
            "GPU-Driven Meshlet + Indirect Draw + IBL",
            "GPU-Driven Meshlet + SSGI + SSR",
            "GPU-Driven Meshlet + SSGI + SSR + DDGI"
        };
        static_assert(std::size(kModeNames) == static_cast<u32>(DawnRenderMode::Count),
                      "kModeNames must cover all DawnRenderMode entries");
        static_assert(std::size(kModeDesc) == static_cast<u32>(DawnRenderMode::Count),
                      "kModeDesc must cover all DawnRenderMode entries");
        // Mode 5 (ObjNormal) visualizes the raw unpacked object-space normal
        // BEFORE the world_matrix 3x3 multiplication. Comparing mode 4 (world)
        // vs mode 5 (object) isolates whether a tilted normal originates from
        // source vertex data (object → fix content pipeline / vertex pull) or
        // from the world transform (world → fix InstanceData world_matrix).
        static const char* kDebugLabel[] = {"Off", "MeshletID", "TriangleID", "MeshID", "Normal", "ObjNormal"};
        bool tabPressed = CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, 48);
        if (tabPressed && !prevTabState_) {
            renderMode_ = static_cast<DawnRenderMode>((static_cast<u8>(renderMode_) + 1) % static_cast<u8>(DawnRenderMode::Count));
            forwardRenderer_.SetDawnRenderMode(static_cast<u32>(renderMode_));
            // TAA history holds the previous mode's HDR image; without a reset
            // it bleeds into the new mode for several frames (visible as a
            // ghost of the prior scene). Mark history slots as invalid so the
            // next AddTAAPass treats the input as a fresh frame.
            PostProcess::ResetTAAHistory();
            std::cerr << "[Mode] " << kModeNames[static_cast<u8>(renderMode_)] << std::endl;
        }
        prevTabState_ = tabPressed;

        const bool vPressed = CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, 9);
        if (vPressed && !prevVState_) {
            if (renderMode_ == DawnRenderMode::MeshletNoIBL) {
                // V key is a Mode-7 affordance: cycles meshlet debug visualization.
                meshletDebugMode_ = (meshletDebugMode_ + 1u) % 6u;
                std::cerr << "[Meshlet] debug visualization mode = " << meshletDebugMode_
                          << " (" << kDebugLabel[meshletDebugMode_] << ")" << std::endl;
            } else if (renderMode_ == DawnRenderMode::MeshletSSGISSR) {
                // Mode 9: V cycles SSGI/SSR sub-mode {Off, SSGIOnly, SSROnly, Both}.
                ssgissrSubmode_ = static_cast<SSGISSRSubmode>((static_cast<u8>(ssgissrSubmode_) + 1u) % 4u);
                static const char* kSubLabel[] = {"Off", "SSGI only", "SSR only", "Both"};
                std::cerr << "[Mode 9] SSGI/SSR sub-mode = " << kSubLabel[static_cast<u8>(ssgissrSubmode_)] << std::endl;
            }
        }
        prevVState_ = vPressed;

        // Auto-reset debug mode when leaving Mode 7 so Mode 8 doesn't inherit
        // hashed-color state via GPUDrivenDrawPipeline::SetDebugMode.
        if (renderMode_ != DawnRenderMode::MeshletNoIBL && meshletDebugMode_ != 0u) {
            meshletDebugMode_ = 0u;
        }

        // Reset Mode 9 sub-mode on exit so the next entry starts at the default (Both).
        if (renderMode_ != DawnRenderMode::MeshletSSGISSR && ssgissrSubmode_ != SSGISSRSubmode::Both) {
            ssgissrSubmode_ = SSGISSRSubmode::Both;
        }

        // Refresh title every frame so the current debug mode is visible
        // alongside the current render mode. Cheap (one snprintf + set_caption).
        char title[256];
        snprintf(title, sizeof(title),
                 "Dawn Forward Renderer | [Tab] Mode | [V] Meshlet Debug: %s | Mode %d: %s — %s",
                 kDebugLabel[meshletDebugMode_],
                 static_cast<u8>(renderMode_), kModeNames[static_cast<u8>(renderMode_)],
                 kModeDesc[static_cast<u8>(renderMode_)]);
        window_.set_caption(title);
    }
#endif

    // Add punctual lights once (ForwardRenderer controls whether they're active via renderMode)
    UpdatePunctualLights();

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
        cameraYaw_ -= mdx * 0.002f;
        cameraPitch_ -= mdy * 0.002f;
    }

    // Tab (keyCode 9) to cycle render mode — edge detected.
    // V (keyCode 86) cycles meshlet debug visualization (mode 7/8 only).
    {
        static const char* kModeNames[] = {"NoEffects", "ShadowOnly", "ShadowAndIBL", "ShadowAndIBLAndPuncLight", "ShadowAndIBLAndPuncLightAndSSR", "Deferred", "LumenDDGI", "MeshletNoIBL", "Meshlet", "MeshletSSGISSR", "MeshletSSGISSRDDGI"};
        static const char* kModeDesc[] = {
            "Directional light only",
            "Directional + Shadow",
            "Directional + Shadow + IBL",
            "Directional + Shadow + IBL + Punctual",
            "Full + Screen-Space Reflections",
            "G-Buffer + Deferred Lighting",
            "G-Buffer + DDGI Global Illumination",
            "Meshlet pipeline — IBL OFF (A/B vs Mode 8)",
            "GPU-Driven Meshlet + Indirect Draw + IBL",
            "GPU-Driven Meshlet + SSGI + SSR",
            "GPU-Driven Meshlet + SSGI + SSR + DDGI"
        };
        static_assert(std::size(kModeNames) == static_cast<u32>(DawnRenderMode::Count),
                      "kModeNames must cover all DawnRenderMode entries");
        static_assert(std::size(kModeDesc) == static_cast<u32>(DawnRenderMode::Count),
                      "kModeDesc must cover all DawnRenderMode entries");
        bool tabPressed = EmscriptenGetKeyState(9);
        if (tabPressed && !prevTabState_) {
            renderMode_ = static_cast<DawnRenderMode>((static_cast<u8>(renderMode_) + 1) % static_cast<u8>(DawnRenderMode::Count));
            forwardRenderer_.SetDawnRenderMode(static_cast<u32>(renderMode_));
            // Drop TAA history so the prior mode's HDR image doesn't bleed in.
            PostProcess::ResetTAAHistory();
            std::cerr << "[Mode] " << kModeNames[static_cast<u8>(renderMode_)] << std::endl;
            EM_ASM_({
                if (window.setRenderMode) {
                    window.setRenderMode($0, UTF8ToString($1), UTF8ToString($2));
                }
            }, static_cast<u8>(renderMode_), kModeNames[static_cast<u8>(renderMode_)], kModeDesc[static_cast<u8>(renderMode_)]);
        }
        prevTabState_ = tabPressed;

        bool vPressed = EmscriptenGetKeyState(86);
        if (vPressed && !prevVState_) {
            if (renderMode_ == DawnRenderMode::MeshletNoIBL) {
                meshletDebugMode_ = (meshletDebugMode_ + 1u) % 6u;
                std::cerr << "[Meshlet] debug visualization mode = " << meshletDebugMode_ << std::endl;
                EM_ASM_({
                    if (window.setMeshletDebug) {
                        window.setMeshletDebug($0);
                    }
                }, meshletDebugMode_);
            } else if (renderMode_ == DawnRenderMode::MeshletSSGISSR) {
                ssgissrSubmode_ = static_cast<SSGISSRSubmode>((static_cast<u8>(ssgissrSubmode_) + 1u) % 4u);
                static const char* kSubLabel[] = {"Off", "SSGI only", "SSR only", "Both"};
                std::cerr << "[Mode 9] SSGI/SSR sub-mode = " << kSubLabel[static_cast<u8>(ssgissrSubmode_)] << std::endl;
                EM_ASM_({
                    if (window.setSSGISSRSubmode) {
                        window.setSSGISSRSubmode($0);
                    }
                }, static_cast<u8>(ssgissrSubmode_));
            }
        }
        prevVState_ = vPressed;

        if (renderMode_ != DawnRenderMode::MeshletNoIBL && meshletDebugMode_ != 0u) {
            meshletDebugMode_ = 0u;
        }
        if (renderMode_ != DawnRenderMode::MeshletSSGISSR && ssgissrSubmode_ != SSGISSRSubmode::Both) {
            ssgissrSubmode_ = SSGISSRSubmode::Both;
        }
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

void Engine_Test::CreatePrepassDepthTexture() {
    if (prepassDepthTexture_ != rhi::handles::INVALID_RESOURCE) return;
    rhi::TextureDesc depthDesc;
    depthDesc.size = {width_, height_, 1};
    depthDesc.format = rhi::DataFormat::D32_Float;
    depthDesc.type = rhi::TextureType::Texture2D;
    depthDesc.mipLevels = 1;
    depthDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
    prepassDepthTexture_ = device_->CreateTexture(depthDesc);
}

void Engine_Test::DestroyPrepassDepthTexture() {
    if (prepassDepthTexture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(prepassDepthTexture_);
        prepassDepthTexture_ = rhi::handles::INVALID_RESOURCE;
    }
}

void Engine_Test::CreateShadowResources() {
    // Shadow depth texture (2048x2048 D32_Float, 4 cascade layers)
    rhi::TextureDesc shadowDesc;
    shadowDesc.size = {2048, 2048, 1};
    shadowDesc.format = rhi::DataFormat::D32_Float;
    shadowDesc.type = rhi::TextureType::Texture2DArray;
    shadowDesc.arraySize = kCascadeCount;
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
#ifdef __EMSCRIPTEN__
        std::string src = dawn::LoadWGSL("ShadowDepth");
#else
        auto platform = device_->GetPlatform();
        std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "ShadowDepth");
        std::string src = LoadShaderSource(shaderPath);
#endif
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
        primal::utl::vector<rhi::VertexInputAttribute> shadowAttrs(5);
        shadowAttrs[0] = {0, 0, rhi::DataFormat::RGB32_Float, 0};
        shadowAttrs[1] = {1, 0, rhi::DataFormat::R32_UInt, 12};
        shadowAttrs[2] = {2, 0, rhi::DataFormat::R32_UInt, 16};
        shadowAttrs[3] = {3, 0, rhi::DataFormat::R32_UInt, 20};
        shadowAttrs[4] = {4, 0, rhi::DataFormat::RG32_Float, 24};
        pipeDesc.vertexAttributes = shadowAttrs;
        primal::utl::vector<rhi::VertexInputBinding> shadowBindings(1);
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
}

primal::math::m4x4 Engine_Test::ComputeLightViewProjection() const {
    primal::math::v3 lightDir = primal::math::v3{0.5f, -0.7f, 0.3f};
    float len = sqrtf(lightDir.x * lightDir.x + lightDir.y * lightDir.y + lightDir.z * lightDir.z);
    lightDir = lightDir / len;

    primal::math::v3 lightTarget = {0.0f, 5.0f, 0.0f};
    primal::math::v3 lightEye = lightTarget - lightDir * 50.0f;
    primal::math::v3 up{0.0f, 1.0f, 0.0f};

    rhimath::m4x4 lightView = rhimath::CreateLookAtMatrix(lightEye, lightTarget, up);
    float orthoSize = 25.0f;
    rhimath::m4x4 lightProj = rhimath::CreateOrthographicMatrix(
        -orthoSize, orthoSize, -orthoSize, orthoSize, 1.0f, 120.0f);

    return lightProj * lightView;
}

void Engine_Test::ComputeCSMViewProjections() {
    // Use simple single light VP for all cascades (debugging cascade computation issue)
    lightVP_ = ComputeLightViewProjection();
    for (u32 i = 0; i < kCascadeCount; ++i) {
        cascadeVPs_[i] = lightVP_;
    }
    // Simple cascade splits based on depth range
    float shaderSplits[4] = {10.0f, 25.0f, 50.0f, 100.0f};
    forwardRenderer_.SetDawnCascadeVPs(cascadeVPs_, shaderSplits);

    // Legacy single VP for fallback
    lightVP_ = cascadeVPs_[0];
}

void Engine_Test::RenderShadowPass(rhi::RHICommandBuffer* cmd) {
    if (shadowPipeline_ == rhi::handles::INVALID_PIPELINE) return;
    if (shadowDepthTexture_ == rhi::handles::INVALID_RESOURCE) return;

    ComputeCSMViewProjections();

    rhi::ViewportDesc vp;
    vp.size = {2048.0f, 2048.0f};
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;

    // Render each cascade layer
    for (u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        rhi::RenderPassDesc passDesc;
        passDesc.depthAttachment.texture = shadowDepthTexture_;
        passDesc.depthAttachment.format = rhi::DataFormat::D32_Float;
        passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
        passDesc.depthAttachment.clearValue.depth = 1.0f;
        passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;
        passDesc.depthAttachment.arrayLayer = cascade;

        cmd->BeginRenderPass(passDesc);
        cmd->SetViewport(vp);
        cmd->SetScissor({{0, 0}, {2048, 2048}});
        cmd->BindGraphicsPipeline(shadowPipeline_);

        if (shadowPerObjectSet_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
            cmd->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, shadowPipelineLayout_, 0, 1, &shadowPerObjectSet_, 0, nullptr);
        }

        u32 meshCount = 0;
        for (u32 i = 0; i < sceneMeshInfos_.size(); ++i) {
            auto& meshInfo = sceneMeshInfos_[i];
            if (!meshInfo.mesh || !meshInfo.mesh->IsValid()) continue;

            if (shadowPerObjectMapped_) {
                auto* p = static_cast<float*>(shadowPerObjectMapped_);
                primal::math::m4x4 identity = rhimath::MatrixIdentity();
                memcpy(p, &identity, 64);
                primal::math::m4x4 worldLightVP = cascadeVPs_[cascade];
                memcpy(p + 16, &worldLightVP, 64);
                device_->SetBufferDirtySize(shadowPerObjectBuf_, 128);
            }

            meshInfo.mesh->Draw(cmd);
            meshCount++;
        }

        cmd->EndRenderPass();
    }

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

// ============================================================
// IBL Precomputation Pipeline
// ============================================================

void Engine_Test::CreateIBLResources() {
    std::cout << "[IBL] Creating IBL resources..." << std::endl;

    // IBL sampler
    rhi::SamplerDesc sDesc;
    sDesc.minFilter = rhi::FilterMode::Linear;
    sDesc.magFilter = rhi::FilterMode::Linear;
    sDesc.addressU = rhi::TextureAddressMode::Clamp;
    sDesc.addressV = rhi::TextureAddressMode::Clamp;
    sDesc.addressW = rhi::TextureAddressMode::Clamp;
    sDesc.comparisonFunc = rhi::ComparisonFunc::Never;
    iblSampler_ = device_->CreateSampler(sDesc);

    // Try to load HDR environment map
#ifdef __EMSCRIPTEN__
    std::string hdrPath = "assets/textures/hdr/sunset.hdr";
#else
    std::string hdrPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/textures/hdr/sunset.hdr";
#endif

    stbi_set_flip_vertically_on_load(true);
    int hdrW, hdrH, hdrComp;
    float* hdrData = stbi_loadf(hdrPath.c_str(), &hdrW, &hdrH, &hdrComp, 4);
    stbi_set_flip_vertically_on_load(false);
    bool hdrFromSTB = (hdrData != nullptr);

    if (!hdrData) {
        std::vector<std::string> altPaths = {
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/textures/hdr/",
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/textures/hdr/",
        };
        for (auto& dir : altPaths) {
            for (auto& name : {"environment.hdr", "env.hdr", "sky.hdr"}) {
                hdrData = stbi_loadf((dir + name).c_str(), &hdrW, &hdrH, &hdrComp, 4);
                if (hdrData) break;
            }
            if (hdrData) break;
        }
    }

    if (!hdrData) {
        // Generate procedural sky gradient as fallback
        constexpr int kProceduralW = 256, kProceduralH = 128;
        hdrW = kProceduralW; hdrH = kProceduralH;
        hdrData = new float[kProceduralW * kProceduralH * 4];
        for (int y = 0; y < kProceduralH; ++y) {
            float v = float(y) / float(kProceduralH - 1); // 0=top(zenith), 1=bottom(nadir)
            for (int x = 0; x < kProceduralW; ++x) {
                float* px = &hdrData[(y * kProceduralW + x) * 4];
                // Soft neutral sky: blue zenith, light horizon, dark below
                // Keep values LOW to avoid dominant IBL specular on surfaces
                px[0] = 0.2f; px[1] = 0.25f; px[2] = 0.45f; px[3] = 1.0f; // base: soft blue
                // Lighter band near horizon (v ~ 0.45-0.55)
                float horizon = expf(-((v - 0.5f) * (v - 0.5f)) * 80.0f);
                px[0] += horizon * 0.25f;
                px[1] += horizon * 0.2f;
                px[2] += horizon * 0.1f;
                // Darken below horizon
                if (v > 0.55f) {
                    float t = (v - 0.55f) / (0.85f - 0.55f);
                    t = t * t * (3.0f - 2.0f * t);
                    px[0] *= 1.0f - t; px[1] *= 1.0f - t; px[2] *= 1.0f - t;
                }
            }
        }
        std::cerr << "[IBL] Using procedural sky gradient (" << kProceduralW << "x" << kProceduralH << ")" << std::endl;
    }

    std::cout << "[IBL] Loaded HDR: " << hdrW << "x" << hdrH << std::endl;

    // Load Hammersley shared code for #include resolution
    std::string hammerSrc = LoadShaderSource("Engine/Graphics/Dawn/shaders/IBL_Hammersley.wgsl");

    // Helper: resolve #include directives
    auto resolveIncludes = [&hammerSrc](std::string& src) {
        size_t pos = src.find("#include \"IBL_Hammersley.wgsl\"");
        if (pos != std::string::npos) src.replace(pos, 32, hammerSrc);
    };

    // Create equirectangular texture from HDR data (RGBA16_Float for filterable sampling)
    rhi::TextureDesc eqDesc;
    eqDesc.size = {(u32)hdrW, (u32)hdrH, 1};
    eqDesc.format = rhi::DataFormat::RGBA16_Float;
    eqDesc.type = rhi::TextureType::Texture2D;
    eqDesc.mipLevels = 1;
    eqDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDest;
    ResourceHandle equirectTex = device_->CreateTexture(eqDesc);
    // Convert float32 HDR → float16 for RGBA16_Float texture
    {
        size_t pixelCount = (size_t)hdrW * hdrH * 4;
        std::vector<uint16_t> halfData(pixelCount);
        for (size_t i = 0; i < pixelCount; ++i) halfData[i] = f32_to_f16(hdrData[i]);
        device_->UpdateTextureData(equirectTex, (const unsigned char*)halfData.data(), 0, 0, 0,
                                   (u32)hdrW, (u32)hdrH, 1, 4 * sizeof(uint16_t) * hdrW, 0);
    }
    if (hdrFromSTB) { stbi_image_free(hdrData); } else { delete[] hdrData; }

    // Create env cubemap (128x128)
    constexpr u32 kCubeSize = 128;
    rhi::TextureDesc cubeDesc;
    cubeDesc.size = {kCubeSize, kCubeSize, 1};
    cubeDesc.format = rhi::DataFormat::RGBA16_Float;
    cubeDesc.type = rhi::TextureType::TextureCube;
    cubeDesc.mipLevels = 1;
    cubeDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
    ResourceHandle envCubeTex = device_->CreateTexture(cubeDesc);

    // ---- Step 1: Equirectangular → Cubemap ----
    {
        // Create 2DArray storage view of cubemap for write
        rhi::TextureViewDesc storageViewDesc{};
        storageViewDesc.texture = envCubeTex;
        storageViewDesc.viewType = rhi::TextureType::Texture2DArray;
        storageViewDesc.firstArraySlice = 0;
        storageViewDesc.arraySize = 6;
        storageViewDesc.format = rhi::DataFormat::RGBA16_Float;
        ResourceHandle envCubeView = device_->CreateTextureView(storageViewDesc);

        std::string shaderSrc = LoadShaderSource("Engine/Graphics/Dawn/shaders/IBL_EquirectangularToCube.wgsl");
        if (shaderSrc.empty()) { std::cerr << "[IBL] Failed to load EquirectToCube shader" << std::endl; return; }

        auto cs = device_->CreateShader(shaderSrc.data(), shaderSrc.size(), rhi::ShaderStage::Compute, "cs_main");

        rhi::DescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = {0, rhi::DescriptorType::SampledImage, 1, rhi::ShaderStage::Compute};
        bindings[1] = {1, rhi::DescriptorType::StorageImage, 1, rhi::ShaderStage::Compute}; bindings[1].isArray = true;
        rhi::DescriptorSetLayoutDesc dslDesc{2, bindings};
        auto dsl = device_->CreateDescriptorSetLayout(dslDesc);

        rhi::PipelineLayoutDesc plDesc; plDesc.setLayoutCount = 1; plDesc.setLayouts = &dsl;
        auto pipelineLayout = device_->CreatePipelineLayout(plDesc);

        rhi::ComputePipelineDesc pipeDesc; pipeDesc.computeShader = cs; pipeDesc.layout = pipelineLayout; pipeDesc.threadGroupSize = {16, 16, 1};
        auto pipeline = device_->CreateComputePipeline(pipeDesc);

        rhi::DescriptorSetDesc dsDesc; dsDesc.layout = dsl;
        auto ds = device_->CreateDescriptorSet(dsDesc);

        rhi::DescriptorImageInfo imgs[2];
        imgs[0].imageView = equirectTex; imgs[0].sampler = iblSampler_;
        imgs[1].imageView = envCubeView; imgs[1].sampler = iblSampler_;
        rhi::WriteDescriptorSet writes[2];
        for (int i = 0; i < 2; ++i) {
            writes[i] = {ds, (u32)i, 0, 1, bindings[i].descriptorType, &imgs[i]};
        }
        device_->UpdateDescriptorSets(2, writes);

        auto cmdBuf = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
        auto* cmd = device_->GetCommandBuffer(cmdBuf);
        if (cmd && cmd->Begin()) {
            cmd->BindComputePipeline(pipeline);
            cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, pipelineLayout, 0, 1, &ds, 0, nullptr);
            u32 groups = (kCubeSize + 15) / 16;
            cmd->Dispatch(groups, groups, 6);
            cmd->End();
        }
        rhi::QueueSubmitInfo submit{}; submit.cmdBuffer = cmdBuf;
        device_->Submit(submit);
        device_->WaitIdle();

        device_->DestroyCommandBuffer(cmdBuf);
        device_->DestroyDescriptorSet(ds);
        device_->DestroyDescriptorSetLayout(dsl);
        device_->DestroyPipelineLayout(pipelineLayout);
        device_->DestroyPipeline(pipeline);
        device_->DestroyTexture(envCubeView);
    }
    std::cout << "[IBL] Equirect→Cube done" << std::endl;
    device_->DestroyTexture(equirectTex);

    // ---- Step 2: Irradiance Convolution ----
    constexpr u32 kIrradSize = 32;
    {
        rhi::TextureDesc irradDesc;
        irradDesc.size = {kIrradSize, kIrradSize, 1};
        irradDesc.format = rhi::DataFormat::RGBA16_Float;
        irradDesc.type = rhi::TextureType::TextureCube;
        irradDesc.mipLevels = 1;
        irradDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
        iblIrradianceTex_ = device_->CreateTexture(irradDesc);

        // Create 2DArray storage view
        rhi::TextureViewDesc storageViewDesc{};
        storageViewDesc.texture = iblIrradianceTex_;
        storageViewDesc.viewType = rhi::TextureType::Texture2DArray;
        storageViewDesc.firstArraySlice = 0;
        storageViewDesc.arraySize = 6;
        storageViewDesc.format = rhi::DataFormat::RGBA16_Float;
        ResourceHandle irradView = device_->CreateTextureView(storageViewDesc);

        std::string shaderSrc = LoadShaderSource("Engine/Graphics/Dawn/shaders/IBL_IrradianceConvolution.wgsl");
        if (shaderSrc.empty()) { std::cerr << "[IBL] Failed to load IrradianceConvolution shader" << std::endl; return; }
        resolveIncludes(shaderSrc);

        auto cs = device_->CreateShader(shaderSrc.data(), shaderSrc.size(), rhi::ShaderStage::Compute, "cs_main");

        rhi::DescriptorSetLayoutBinding bindings[4]{};
        bindings[0] = {0, rhi::DescriptorType::SampledImage, 1, rhi::ShaderStage::Compute}; bindings[0].isCube = true;
        bindings[1] = {1, rhi::DescriptorType::StorageImage, 1, rhi::ShaderStage::Compute}; bindings[1].isArray = true;
        bindings[2] = {2, rhi::DescriptorType::Sampler, 1, rhi::ShaderStage::Compute};
        bindings[3] = {3, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Compute};
        rhi::DescriptorSetLayoutDesc dslDesc{4, bindings};
        auto dsl = device_->CreateDescriptorSetLayout(dslDesc);

        rhi::PipelineLayoutDesc plDesc; plDesc.setLayoutCount = 1; plDesc.setLayouts = &dsl;
        auto pipelineLayout = device_->CreatePipelineLayout(plDesc);

        rhi::ComputePipelineDesc pipeDesc; pipeDesc.computeShader = cs; pipeDesc.layout = pipelineLayout; pipeDesc.threadGroupSize = {16, 16, 1};
        auto pipeline = device_->CreateComputePipeline(pipeDesc);

        // Uniform: faceSize(u32) + pad(12 bytes) = 16 bytes
        rhi::BufferDesc ubDesc{}; ubDesc.size = 16; ubDesc.type = rhi::BufferType::Constant;
        ubDesc.usage = rhi::GPUMemoryUsage::Dynamic; ubDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        ResourceHandle ub = device_->CreateBuffer(ubDesc);
        void* ubMapped = device_->MapBuffer(ub, 0, 16);
        if (ubMapped) { auto* p = static_cast<u32*>(ubMapped); p[0] = kIrradSize; p[1] = 0; p[2] = 0; p[3] = 0; device_->SetBufferDirtySize(ub, 16); }

        rhi::DescriptorSetDesc dsDesc; dsDesc.layout = dsl;
        auto ds = device_->CreateDescriptorSet(dsDesc);

        rhi::DescriptorImageInfo imgs[3];
        imgs[0].imageView = envCubeTex; imgs[0].sampler = iblSampler_;
        imgs[1].imageView = irradView;  imgs[1].sampler = iblSampler_;
        imgs[2].imageView = envCubeTex; imgs[2].sampler = iblSampler_;
        rhi::DescriptorBufferInfo bufInfo; bufInfo.buffer = ub; bufInfo.offset = 0; bufInfo.range = 16;
        rhi::WriteDescriptorSet writes[4];
        writes[0] = {ds, 0, 0, 1, rhi::DescriptorType::SampledImage, &imgs[0]};
        writes[1] = {ds, 1, 0, 1, rhi::DescriptorType::StorageImage, &imgs[1]};
        writes[2] = {ds, 2, 0, 1, rhi::DescriptorType::Sampler, &imgs[2]};
        writes[3] = {ds, 3, 0, 1, rhi::DescriptorType::UniformBuffer, nullptr, &bufInfo};
        device_->UpdateDescriptorSets(4, writes);

        auto cmdBuf = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
        auto* cmd = device_->GetCommandBuffer(cmdBuf);
        if (cmd && cmd->Begin()) {
            cmd->BindComputePipeline(pipeline);
            cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, pipelineLayout, 0, 1, &ds, 0, nullptr);
            u32 groups = (kIrradSize + 15) / 16;
            cmd->Dispatch(groups, groups, 6);
            cmd->End();
        }
        rhi::QueueSubmitInfo submit{}; submit.cmdBuffer = cmdBuf;
        device_->Submit(submit);
        device_->WaitIdle();

        device_->DestroyCommandBuffer(cmdBuf);
        device_->UnmapBuffer(ub); device_->DestroyBuffer(ub);
        device_->DestroyDescriptorSet(ds); device_->DestroyDescriptorSetLayout(dsl);
        device_->DestroyPipelineLayout(pipelineLayout); device_->DestroyPipeline(pipeline);
        device_->DestroyTexture(irradView);
    }
    std::cout << "[IBL] Irradiance convolution done" << std::endl;

    // ---- Step 3: Specular Prefilter (5 mip levels) ----
    {
        rhi::TextureDesc pfDesc;
        pfDesc.size = {kCubeSize, kCubeSize, 1};
        pfDesc.format = rhi::DataFormat::RGBA16_Float;
        pfDesc.type = rhi::TextureType::TextureCube;
        pfDesc.mipLevels = 5;
        pfDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
        iblPrefilterTex_ = device_->CreateTexture(pfDesc);

        std::string shaderSrc = LoadShaderSource("Engine/Graphics/Dawn/shaders/IBL_SpecularPrefilter.wgsl");
        if (shaderSrc.empty()) { std::cerr << "[IBL] Failed to load SpecularPrefilter shader" << std::endl; return; }
        resolveIncludes(shaderSrc);

        auto cs = device_->CreateShader(shaderSrc.data(), shaderSrc.size(), rhi::ShaderStage::Compute, "cs_main");

        rhi::DescriptorSetLayoutBinding bindings[4]{};
        bindings[0] = {0, rhi::DescriptorType::SampledImage, 1, rhi::ShaderStage::Compute}; bindings[0].isCube = true;
        bindings[1] = {1, rhi::DescriptorType::StorageImage, 1, rhi::ShaderStage::Compute}; bindings[1].isArray = true;
        bindings[2] = {2, rhi::DescriptorType::Sampler, 1, rhi::ShaderStage::Compute};
        bindings[3] = {3, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Compute};
        rhi::DescriptorSetLayoutDesc dslDesc{4, bindings};
        auto dsl = device_->CreateDescriptorSetLayout(dslDesc);

        rhi::PipelineLayoutDesc plDesc; plDesc.setLayoutCount = 1; plDesc.setLayouts = &dsl;
        auto pipelineLayout = device_->CreatePipelineLayout(plDesc);

        rhi::ComputePipelineDesc pipeDesc; pipeDesc.computeShader = cs; pipeDesc.layout = pipelineLayout; pipeDesc.threadGroupSize = {16, 16, 1};
        auto pipeline = device_->CreateComputePipeline(pipeDesc);

        rhi::BufferDesc ubDesc{}; ubDesc.size = 32; ubDesc.type = rhi::BufferType::Constant;
        ubDesc.usage = rhi::GPUMemoryUsage::Dynamic; ubDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        ResourceHandle ub = device_->CreateBuffer(ubDesc);
        void* ubMapped = device_->MapBuffer(ub, 0, 32);

        rhi::DescriptorSetDesc dsDesc; dsDesc.layout = dsl;
        auto ds = device_->CreateDescriptorSet(dsDesc);

        for (u32 mip = 0; mip < 5; ++mip) {
            u32 mipSize = kCubeSize >> mip;
            float roughness = (float)mip / 4.0f;

            // Create 2DArray storage view for this mip level
            rhi::TextureViewDesc storageViewDesc{};
            storageViewDesc.texture = iblPrefilterTex_;
            storageViewDesc.viewType = rhi::TextureType::Texture2DArray;
            storageViewDesc.firstArraySlice = 0;
            storageViewDesc.arraySize = 6;
            storageViewDesc.format = rhi::DataFormat::RGBA16_Float;
            storageViewDesc.mostDetailedMip = mip;
            storageViewDesc.mipCount = 1;
            ResourceHandle mipView = device_->CreateTextureView(storageViewDesc);

            // Update uniform: faceSize(u32), pad(u32), roughness(f32), srcResolution(f32)
            if (ubMapped) {
                auto* p = static_cast<u32*>(ubMapped);
                p[0] = mipSize;
                p[1] = 0;
                float* fp = reinterpret_cast<float*>(p + 2);
                fp[0] = roughness;
                fp[1] = (float)kCubeSize;
                device_->SetBufferDirtySize(ub, 32);
            }

            rhi::DescriptorImageInfo imgs[3];
            imgs[0].imageView = envCubeTex; imgs[0].sampler = iblSampler_;
            imgs[1].imageView = mipView;    imgs[1].sampler = iblSampler_;
            imgs[2].imageView = envCubeTex; imgs[2].sampler = iblSampler_;
            rhi::DescriptorBufferInfo bufInfo; bufInfo.buffer = ub; bufInfo.offset = 0; bufInfo.range = 32;
            rhi::WriteDescriptorSet writes[4];
            writes[0] = {ds, 0, 0, 1, rhi::DescriptorType::SampledImage, &imgs[0]};
            writes[1] = {ds, 1, 0, 1, rhi::DescriptorType::StorageImage, &imgs[1]};
            writes[2] = {ds, 2, 0, 1, rhi::DescriptorType::Sampler, &imgs[2]};
            writes[3] = {ds, 3, 0, 1, rhi::DescriptorType::UniformBuffer, nullptr, &bufInfo};
            device_->UpdateDescriptorSets(4, writes);

            auto cmdBuf = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
            auto* cmd = device_->GetCommandBuffer(cmdBuf);
            if (cmd && cmd->Begin()) {
                cmd->BindComputePipeline(pipeline);
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, pipelineLayout, 0, 1, &ds, 0, nullptr);
                u32 groups = (mipSize + 15) / 16;
                cmd->Dispatch(groups, groups, 6);
                cmd->End();
            }
            rhi::QueueSubmitInfo submit{}; submit.cmdBuffer = cmdBuf;
            device_->Submit(submit);
            device_->WaitIdle();
            device_->DestroyCommandBuffer(cmdBuf);
            device_->DestroyTexture(mipView);
        }

        device_->UnmapBuffer(ub); device_->DestroyBuffer(ub);
        device_->DestroyDescriptorSet(ds); device_->DestroyDescriptorSetLayout(dsl);
        device_->DestroyPipelineLayout(pipelineLayout); device_->DestroyPipeline(pipeline);
    }
    std::cout << "[IBL] Specular prefilter done" << std::endl;

    // ---- Step 4: BRDF Integration LUT (512x512) ----
    {
        constexpr u32 kLUTSize = 512;
        rhi::TextureDesc lutDesc;
        lutDesc.size = {kLUTSize, kLUTSize, 1};
        lutDesc.format = rhi::DataFormat::RGBA16_Float;
        lutDesc.type = rhi::TextureType::Texture2D;
        lutDesc.mipLevels = 1;
        lutDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
        iblBRDFLUTTex_ = device_->CreateTexture(lutDesc);

        std::string shaderSrc = LoadShaderSource("Engine/Graphics/Dawn/shaders/IBL_BRDFIntegration.wgsl");
        if (shaderSrc.empty()) { std::cerr << "[IBL] Failed to load BRDFIntegration shader" << std::endl; return; }
        resolveIncludes(shaderSrc);

        auto cs = device_->CreateShader(shaderSrc.data(), shaderSrc.size(), rhi::ShaderStage::Compute, "cs_main");

        rhi::DescriptorSetLayoutBinding binding{};
        binding.binding = 0; binding.descriptorType = rhi::DescriptorType::StorageImage;
        binding.descriptorCount = 1; binding.stageFlags = rhi::ShaderStage::Compute;
        rhi::DescriptorSetLayoutDesc dslDesc{1, &binding};
        auto dsl = device_->CreateDescriptorSetLayout(dslDesc);

        rhi::PipelineLayoutDesc plDesc; plDesc.setLayoutCount = 1; plDesc.setLayouts = &dsl;
        auto pipelineLayout = device_->CreatePipelineLayout(plDesc);

        rhi::ComputePipelineDesc pipeDesc; pipeDesc.computeShader = cs; pipeDesc.layout = pipelineLayout; pipeDesc.threadGroupSize = {16, 16, 1};
        auto pipeline = device_->CreateComputePipeline(pipeDesc);

        rhi::DescriptorSetDesc dsDesc; dsDesc.layout = dsl;
        auto ds = device_->CreateDescriptorSet(dsDesc);

        rhi::DescriptorImageInfo imgInfo; imgInfo.imageView = iblBRDFLUTTex_; imgInfo.sampler = iblSampler_;
        rhi::WriteDescriptorSet write;
        write.dstSet = ds; write.dstBinding = 0; write.descriptorCount = 1;
        write.descriptorType = rhi::DescriptorType::StorageImage; write.imageInfo = &imgInfo;
        device_->UpdateDescriptorSets(1, &write);

        auto cmdBuf = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
        auto* cmd = device_->GetCommandBuffer(cmdBuf);
        if (cmd && cmd->Begin()) {
            cmd->BindComputePipeline(pipeline);
            cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute, pipelineLayout, 0, 1, &ds, 0, nullptr);
            u32 groups = (kLUTSize + 15) / 16;
            cmd->Dispatch(groups, groups, 1);
            cmd->End();
        }
        rhi::QueueSubmitInfo submit{}; submit.cmdBuffer = cmdBuf;
        device_->Submit(submit);
        device_->WaitIdle();

        device_->DestroyCommandBuffer(cmdBuf);
        device_->DestroyDescriptorSet(ds); device_->DestroyDescriptorSetLayout(dsl);
        device_->DestroyPipelineLayout(pipelineLayout); device_->DestroyPipeline(pipeline);
    }
    std::cout << "[IBL] BRDF LUT done" << std::endl;

    device_->DestroyTexture(envCubeTex);

    forwardRenderer_.SetDawnIBLResources(iblIrradianceTex_, iblPrefilterTex_, iblBRDFLUTTex_, iblSampler_);
    std::cout << "[IBL] All IBL resources created and bound" << std::endl;
}

void Engine_Test::RunIBLCompute(ResourceHandle, u32) {
    // IBL compute is done during initialization, not per-frame
}

void Engine_Test::UpdatePunctualLights() {
    if (punctualLightsAdded_) return;
    // Colored lights placed inside Sponza's shadowed areas (arches, corridors)
    RenderLight greenLight, orangeLight, purpleLight, spotLight;

    // Green — left archway corridor
    greenLight.type = LightType::Point;
    greenLight.position = primal::math::v3{-6.0f, 2.5f, 0.0f};
    greenLight.color = primal::math::v3{0.1f, 1.0f, 0.3f};
    greenLight.intensity = 4.0f;
    greenLight.range = 6.0f;
    scene_.AddLight(greenLight);

    // Orange — right archway corridor
    orangeLight.type = LightType::Point;
    orangeLight.position = primal::math::v3{6.0f, 2.5f, 0.0f};
    orangeLight.color = primal::math::v3{1.0f, 0.5f, 0.1f};
    orangeLight.intensity = 4.0f;
    orangeLight.range = 6.0f;
    scene_.AddLight(orangeLight);

    // Purple — under central upper gallery
    purpleLight.type = LightType::Point;
    purpleLight.position = primal::math::v3{0.0f, 2.5f, 3.0f};
    purpleLight.color = primal::math::v3{0.6f, 0.1f, 1.0f};
    purpleLight.intensity = 3.0f;
    purpleLight.range = 5.0f;
    scene_.AddLight(purpleLight);

    // Warm spot — inside arch pointing at back wall
    spotLight.type = LightType::Spot;
    spotLight.position = primal::math::v3{0.0f, 6.0f, -3.0f};
    spotLight.direction = primal::math::v3{0.0f, -1.0f, 1.0f};
    spotLight.color = primal::math::v3{1.0f, 0.9f, 0.5f};
    spotLight.intensity = 5.0f;
    spotLight.range = 8.0f;
    spotLight.outerCone = 0.7f;
    spotLight.innerCone = 0.9f;
    scene_.AddLight(spotLight);

    punctualLightsAdded_ = true;
}

// ============================================================
// Meshlet pipeline (mode 7/8)
// ============================================================
// Nanite sources compile for WASM (Metal-only includes are guarded by
// #ifdef __APPLE__). Mesh asset / GPU mesh data comes through stubs that
// return empty until ContentToEngine.cpp is ported to wasm32.

bool Engine_Test::InitializeMeshletPipeline() {
    if (meshletInitialized_) return true;
    if (!device_) return false;

    // 0. NaniteResourceManager — must be initialized BEFORE RenderSceneSnapshot
    //    tries to GetOrCreateResource(geometry_content_id). Without this, the
    //    manager's device_ is null, every lookup returns nullptr, and every
    //    instance ends up with cluster_count=0 — which makes UpdateGeometryData
    //    bail early (totalMeshlets == 0), so cluster_map_buffer_ and
    //    global_instance_data_buffer_ never get created.
    auto& resourceManager = primal::graphics::nanite::NaniteResourceManager::Get();
    if (!resourceManager.Initialize(device_)) {
        std::cerr << "[Meshlet] NaniteResourceManager init failed" << std::endl;
        return false;
    }

    // 0b. Pre-check: scan mesh assets. We need either pre-baked MSHL sections
    // OR (failing that) at least one asset with index data — the runtime
    // synthesizer chunks the index buffer into meshlets on demand. If every
    // asset is empty (no indices at all), the pipeline can't draw anything.
    {
        u32 assetsUsable = 0;
        for (const auto& meshInfo : sceneMeshInfos_) {
            graphics::rhi::RHIMeshAsset asset;
            if (!primal::content::get_rhi_mesh_asset(meshInfo.meshEntityId, asset)) continue;
            if (!asset.meshlets.empty() || asset.num_indices > 0) {
                assetsUsable++;
            }
        }
        if (assetsUsable == 0) {
            std::cerr << "[Meshlet] No usable mesh assets among the "
                      << sceneMeshInfos_.size() << " loaded. "
                      << "Mode 7 needs either pre-baked MSHL data or index buffers "
                      << "we can synthesize from. Falling back to NoEffects." << std::endl;
            return false;
        }
        std::cerr << "[Meshlet] " << assetsUsable << " / " << sceneMeshInfos_.size()
                  << " meshes usable (MSHL or index-buffer synthesizable)" << std::endl;
    }

    // 1. GPUDrivenDrawPipeline (singleton)
    auto& gpuDrawPipeline = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
    primal::graphics::nanite::BinningConfig binningConfig{};
    binningConfig.bin_size = 64;
    binningConfig.max_bins_per_frame = 4096;
    binningConfig.max_clusters_per_bin = 256;
    binningConfig.enable_spatial_sorting = true;

    primal::graphics::nanite::VisibilityBufferConfig visibilityConfig{};
    visibilityConfig.width = width_;
    visibilityConfig.height = height_;
    visibilityConfig.format = rhi::DataFormat::R32_UInt;
    visibilityConfig.enable_depth = true;

    if (!gpuDrawPipeline.Initialize(device_, binningConfig, visibilityConfig)) {
        std::cerr << "[Meshlet] GPUDrivenDrawPipeline init failed" << std::endl;
        return false;
    }

    // 2. GPUCullingPipeline (singleton)
    auto& cullingPipeline = primal::graphics::nanite::GPUCullingPipeline::Get();
    primal::graphics::nanite::CullingConfig cullingConfig;
    cullingConfig.max_clusters_per_dispatch = 100000;
    cullingConfig.max_instances_per_dispatch = 10000;
    cullingConfig.enable_streaming_feedback = false;
    cullingConfig.enable_occlusion_culling = false;
    cullingConfig.enable_lod_selection = false;
    cullingConfig.enable_debug_output = false;

    std::cerr << "[Meshlet] Initializing GPUCullingPipeline..." << std::endl;
    if (!cullingPipeline.Initialize(device_, cullingConfig)) {
        std::cerr << "[Meshlet] GPUCullingPipeline init failed" << std::endl;
        return false;
    }
    std::cerr << "[Meshlet] GPUCullingPipeline initialized" << std::endl;

    // Cross-wire draw ↔ cull (meshlet buffer access both ways).
    gpuDrawPipeline.SetCullingPipeline(&cullingPipeline);
    cullingPipeline.SetGPUDrawPipeline(&gpuDrawPipeline);

    // 3. HZBSystem (owned by test)
    meshletHZBSystem_ = new primal::graphics::nanite::HZBSystem();
    primal::graphics::nanite::HZBSystem::Config hzbConfig;
    hzbConfig.max_width = width_;
    hzbConfig.max_height = height_;
    hzbConfig.min_mip_size = 8;
    hzbConfig.enable_compression = false;
    hzbConfig.generate_on_gpu = true;
    std::cerr << "[Meshlet] Initializing HZBSystem..." << std::endl;
    if (!meshletHZBSystem_->Initialize(device_, hzbConfig)) {
        std::cerr << "[Meshlet] HZBSystem init failed" << std::endl;
        delete meshletHZBSystem_;
        meshletHZBSystem_ = nullptr;
        return false;
    }
    std::cerr << "[Meshlet] HZBSystem initialized" << std::endl;

    gpuDrawPipeline.SetHZBSystem(meshletHZBSystem_);
    cullingPipeline.SetHZBSystem(meshletHZBSystem_);

    // 4. Material registry — register all Sponza materials, build + upload.
    meshletMaterialRegistry_ = new primal::graphics::nanite::GPUMaterialRegistry();
    u32 registeredCount = 0;
    for (auto& meshInfo : sceneMeshInfos_) {
        if (meshInfo.materialInstance) {
            auto matID = meshletMaterialRegistry_->RegisterMaterial(meshInfo.materialInstance.get());
            if (matID != primal::graphics::nanite::GPUMaterialRegistry::INVALID_MATERIAL_ID) {
                meshInfo.gpuMaterialId = matID;
                registeredCount++;
            }
        }
    }
    std::cerr << "[Meshlet] Registered " << registeredCount << " materials" << std::endl;

    // Patch RenderProxy.materialId on every proxy — at LoadScene time we stored
    // entity_id there as a placeholder, but the GPU material buffer is indexed
    // by the registry's slot ID (0,1,2,...). Without this patch the fragment
    // shader reads material_data[entity_id * 48] — OOB, returns zeros, and the
    // GBuffer albedo defaults to white (vec4(1.0)).
    for (auto& meshInfo : sceneMeshInfos_) {
        if (meshInfo.materialInstance && meshInfo.gpuMaterialId != primal::id::invalid_id) {
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

    // Rebuild materials_ so it is keyed by gpuMaterialId (matching the patched
    // proxy.materialId). LoadScene keyed materials_ by entity_id; once we patch
    // proxy.materialId above, ForwardRenderer::OpaquePass / TransparentPass /
    // ShadowPass / DepthPrePass / ReflectionPass all do
    //   materials.find(proxy->materialId)
    // and would otherwise read mesh[N-1]'s MI for mesh[N] — the off-by-one
    // "texture mixing" bug. ForwardPBR modes (0-4) depend on this CPU-side
    // lookup; GBuffer/Meshlet modes (5-8) read the GPU material buffer directly
    // and were never affected.
    {
        std::unordered_map<primal::id::id_type, std::shared_ptr<primal::graphics::MaterialInstance>> rebuilt;
        rebuilt.reserve(materials_.size());
        for (auto& meshInfo : sceneMeshInfos_) {
            if (meshInfo.materialInstance && meshInfo.gpuMaterialId != primal::id::invalid_id) {
                rebuilt[meshInfo.gpuMaterialId] = meshInfo.materialInstance;
            }
        }
        materials_ = std::move(rebuilt);
    }

    auto buildJob = meshletMaterialRegistry_->BuildAsync(device_);
    buildJob.Wait();
    if (!meshletMaterialRegistry_->UploadToGPU(device_)) {
        std::cerr << "[Meshlet] Material upload failed" << std::endl;
    } else {
        gpuDrawPipeline.SetMaterialDataBuffer(meshletMaterialRegistry_->GetMaterialDataBuffer());

        // Texture sampler — Wrap addressing to match the Metal test (Sponza uses repeat).
        rhi::SamplerDesc samplerDesc{};
        samplerDesc.minFilter = rhi::FilterMode::Linear;
        samplerDesc.magFilter = rhi::FilterMode::Linear;
        samplerDesc.mipFilter = rhi::FilterMode::Linear;
        samplerDesc.addressU = rhi::TextureAddressMode::Wrap;
        samplerDesc.addressV = rhi::TextureAddressMode::Wrap;
        samplerDesc.addressW = rhi::TextureAddressMode::Wrap;
        samplerDesc.maxAnisotropy = 1;
        samplerDesc.minLod = 0.0f;
        samplerDesc.maxLod = 100.0f;
        samplerDesc.comparisonFunc = rhi::ComparisonFunc::Never;
        rhi::SamplerHandle texSampler = device_->CreateSampler(samplerDesc);

        gpuDrawPipeline.SetTextureArrays(
            meshletMaterialRegistry_->GetAlbedoTextureArray(),
            meshletMaterialRegistry_->GetNormalTextureArray(),
            meshletMaterialRegistry_->GetORMTextureArray(),
            texSampler);
    }

    // 5. Scene snapshot — converts RenderScene to GPU instance/cluster buffers.
    if (!meshletSceneSnapshot_.Initialize(device_, /*max_instances=*/1000, /*max_clusters=*/100000)) {
        std::cerr << "[Meshlet] RenderSceneSnapshot init failed" << std::endl;
        return false;
    }
    if (!meshletSceneSnapshot_.Rebind(scene_)) {
        std::cerr << "[Meshlet] RenderSceneSnapshot rebind failed" << std::endl;
        return false;
    }

    // 6. Shadow resources (uses 2 cascades internally — ShadowFrameResources has [2]).
    if (!gpuDrawPipeline.InitializeShadowResources(
            (u32)sceneMeshInfos_.size(), /*max_clusters=*/100000)) {
        std::cerr << "[Meshlet] Shadow resources init failed" << std::endl;
    }

    meshletInitialized_ = true;
    std::cerr << "[Meshlet] Pipeline initialized" << std::endl;
    return true;
}

void Engine_Test::ShutdownMeshletPipeline() {
    if (!meshletInitialized_) return;

    auto& gpuDrawPipeline = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
    auto& cullingPipeline = primal::graphics::nanite::GPUCullingPipeline::Get();

    gpuDrawPipeline.ShutdownShadowResources();
    meshletSceneSnapshot_.Shutdown();

    if (meshletMaterialRegistry_) {
        delete meshletMaterialRegistry_;
        meshletMaterialRegistry_ = nullptr;
    }
    if (meshletHZBSystem_) {
        meshletHZBSystem_->Shutdown();
        delete meshletHZBSystem_;
        meshletHZBSystem_ = nullptr;
    }

    cullingPipeline.Shutdown();
    gpuDrawPipeline.Shutdown();

    meshletInitialized_ = false;
}

// ============================================================
// Mode 10: DDGI (Phase A) — CPU bake + LumenDDGIPass init
// ============================================================
// BuildProbeBakingScene merges CPU-side mesh vertex/index arrays from
// sceneMeshInfos_ into flat arrays suitable for the BVH ray tracer in
// StaticProbeBaker. The merged buffers are static so the pointers handed
// to ProbeBakingScene remain valid for the duration of the blocking Bake()
// call (Phase A only invokes this once per Mode 10 entry).

void Engine_Test::BuildProbeBakingScene(primal::graphics::lumen::ProbeBakingScene& scene) {
    // RHIMeshAsset.position_buffer is tightly packed f32x3 (12 bytes/vertex)
    // per ContentToEngine.cpp. Index buffer is u16 or u32 — normalize to u32.
    static std::vector<primal::math::v3> merged_vertices;  // static: outlives the call
    static std::vector<u32>              merged_indices;
    merged_vertices.clear();
    merged_indices.clear();

    for (const auto& meshInfo : sceneMeshInfos_) {
        if (meshInfo.meshEntityId == primal::id::invalid_id) continue;

        primal::graphics::rhi::RHIMeshAsset asset;
        if (!primal::content::get_rhi_mesh_asset(meshInfo.meshEntityId, asset)) continue;
        if (asset.num_vertices == 0 || asset.num_indices == 0) continue;

        const u32 vertex_base = (u32)merged_vertices.size();
        const u32 index_base  = (u32)merged_indices.size();

        // Append positions (asset.position_buffer is f32x3 tightly packed).
    const primal::math::v3* positions = reinterpret_cast<const primal::math::v3*>(asset.position_buffer.data());
        merged_vertices.insert(merged_vertices.end(), positions, positions + asset.num_vertices);

        // Append indices with vertex-base offset; expand u16 -> u32 if needed.
        const u32 base_index_offset = vertex_base;
        if (asset.index_size == 4) {
            const u32* idx = reinterpret_cast<const u32*>(asset.index_buffer.data());
            for (u32 i = 0; i < asset.num_indices; ++i) {
                merged_indices.push_back(idx[i] + base_index_offset);
            }
        } else if (asset.index_size == 2) {
            const u16* idx = reinterpret_cast<const u16*>(asset.index_buffer.data());
            for (u32 i = 0; i < asset.num_indices; ++i) {
                merged_indices.push_back((u32)idx[i] + base_index_offset);
            }
        } else {
            // Unknown index format — skip this asset and roll back vertex append.
            std::cerr << "[Mode10] Skipping mesh " << meshInfo.meshEntityId
                      << " with unknown index_size=" << asset.index_size << std::endl;
            merged_vertices.resize(vertex_base);
            continue;
        }

        // Defensive: if no indices actually got pushed, roll back vertices too.
        if (merged_indices.size() == index_base) {
            merged_vertices.resize(vertex_base);
        }
    }

    scene.vertices      = merged_vertices.data();
    scene.vertex_count  = (u32)merged_vertices.size();
    scene.indices       = merged_indices.data();
    scene.index_count   = (u32)merged_indices.size();

    // Match the directional light configured at TestDawnForwardRenderer.cpp:302-307.
    scene.light_direction = primal::math::v3{0.5f, -0.7f, 0.3f};
    scene.light_color     = primal::math::v3{1.0f, 0.95f, 0.9f};
    scene.light_intensity = 3.0f;
    // Matches DDGI_SKY_COLOR in Engine/Graphics/Dawn/shaders/Lumen/DDGITraceRays.wgsl:38.
    scene.sky_color       = primal::math::v3{0.3f, 0.3f, 0.35f};

    std::cout << "[Mode10] BuildProbeBakingScene: "
              << scene.vertex_count << " verts, "
              << scene.index_count << " indices across "
              << sceneMeshInfos_.size() << " meshes" << std::endl;
}

void Engine_Test::InitializeDDGIForMode10() {
    if (ddgiEnabled_) return;  // already initialized
    if (!device_) return;

    std::cout << "[Mode10] Initializing DDGI..." << std::endl;

    // 1. Create + bake StaticProbeVolume (or load from cache).
    staticProbeVolume_ = std::make_unique<primal::graphics::lumen::StaticProbeVolume>();
    primal::graphics::lumen::StaticProbeParams vol_params;  // defaults: 16x8x16, spacing 4, origin 0

    // Initialize unconditionally so device_ is set on the volume — required by
    // UploadToGPU. LoadFromFile overwrites params_ + data vectors but does not
    // touch device_; without this call the cache-hit path leaves device_ null
    // and UploadToGPU fails.
    if (!staticProbeVolume_->Initialize(device_, vol_params)) {
        std::cerr << "[Mode10] StaticProbeVolume init failed - DDGI disabled" << std::endl;
        staticProbeVolume_.reset();
        return;
    }

    const char* cache_path = "mode10_ddgi_cache.spch";
    if (staticProbeVolume_->LoadFromFile(cache_path)) {
        std::cout << "[Mode10] Loaded DDGI cache from " << cache_path << std::endl;
    } else {
        primal::graphics::lumen::ProbeBakingScene scene;
        BuildProbeBakingScene(scene);

        if (scene.vertex_count == 0 || scene.index_count == 0) {
            std::cerr << "[Mode10] Empty bake scene - DDGI disabled" << std::endl;
            staticProbeVolume_.reset();
            return;
        }

        primal::graphics::lumen::ProbeBakingParams params;
        params.rays_per_probe    = 64;       // match DDGIRuntimeParams default
        params.bounce_count      = 3;
        params.ray_max_distance  = 50.0f;    // match DDGIRuntimeParams default

        std::cout << "[Mode10] Baking DDGI probes (CPU BVH, blocking)..." << std::endl;
        const auto bake_start = std::chrono::steady_clock::now();
        bool ok = primal::graphics::lumen::StaticProbeBaker::Bake(*staticProbeVolume_, scene, params);
        const auto bake_end = std::chrono::steady_clock::now();
        const double bake_seconds = std::chrono::duration<double>(bake_end - bake_start).count();
        std::cout << "[Mode10] Bake " << (ok ? "completed" : "FAILED")
                  << " in " << bake_seconds << "s" << std::endl;

        if (!ok) {
            std::cerr << "[Mode10] Bake failed - DDGI disabled" << std::endl;
            staticProbeVolume_.reset();
            return;
        }

        staticProbeVolume_->SaveToFile(cache_path);
    }

    if (!staticProbeVolume_->UploadToGPU()) {
        std::cerr << "[Mode10] UploadToGPU failed - DDGI disabled" << std::endl;
        staticProbeVolume_.reset();
        return;
    }

    // 2. Initialize LumenDDGIPass with default params (matches StaticProbeParams defaults).
    ddgiPass_ = std::make_unique<primal::graphics::lumen::LumenDDGIPass>();
    if (!ddgiPass_->Initialize(device_)) {
        std::cerr << "[Mode10] LumenDDGIPass init failed - DDGI disabled" << std::endl;
        ddgiPass_.reset();
        staticProbeVolume_.reset();
        return;
    }
    ddgiPass_->SetStaticProbeVolume(staticProbeVolume_.get());

    // Per-frame RenderGraph for DDGI dispatch. Allocated once, cleared/reused
    // each frame. Cannot share renderGraph_ because that one Executes at line
    // ~1103 (after deferred lighting), but DDGI must run between the meshlet
    // GBuffer draw and deferred lighting.
    ddgiGraph_ = std::make_unique<primal::graphics::rendergraph::RenderGraph>(*device_);
    if (!ddgiGraph_) {
        std::cerr << "[Mode10] DDGI RenderGraph allocation failed — DDGI disabled" << std::endl;
        ddgiPass_->Shutdown();
        ddgiPass_.reset();
        staticProbeVolume_.reset();
        return;
    }

    // 3. Allocate half-res RGBA16F indirect texture (GIGather output).
    //    WGSL binding 13 reads this as texture_2d<f32>; RGBA16F matches Lumen convention.
    {
        rhi::TextureDesc desc{};
        desc.size       = {width_ / 2, height_ / 2, 1u};
        desc.format     = rhi::DataFormat::RGBA16_Float;
        desc.type       = rhi::TextureType::Texture2D;
        desc.mipLevels  = 1u;
        desc.arraySize  = 1u;
        // UnorderedAccess = storage write (GIGather writes via textureStore).
        // ShaderResource  = downstream DeferredLighting samples it.
        desc.usage      = rhi::TextureUsage::ShaderResource |
                          rhi::TextureUsage::UnorderedAccess;
        desc.name       = "DDGI_GIIndirect";
        giIndirectTexture_ = device_->CreateTexture(desc);
        if (giIndirectTexture_ == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[Mode10] Failed to create giIndirectTexture_ - DDGI disabled" << std::endl;
            ddgiPass_.reset();
            staticProbeVolume_.reset();
            return;
        }
    }

    // 4. Allocate previous-frame HDR texture (DDGI trace radiance feedback).
    //    Reuse Mode 9's hdr desc verbatim so dimensions/format match hdrTexture_.
    {
        rhi::TextureDesc desc = hdrDesc_;
        desc.name              = "DDGI_PrevHDR";
        prevHdrTexture_ = device_->CreateTexture(desc);
        if (prevHdrTexture_ == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[Mode10] Failed to create prevHdrTexture_ - DDGI disabled" << std::endl;
            device_->DestroyTexture(giIndirectTexture_);
            giIndirectTexture_ = rhi::handles::INVALID_RESOURCE;
            ddgiPass_.reset();
            staticProbeVolume_.reset();
            return;
        }
    }

    // 5. Create inline GIGather compute pipeline (mirrors TestNaniteStreamingPipeline pattern).
    //    WGSL bindings (see DDGIGIGather.wgsl):
    //      0: texture_depth_2d  (gbuffer depth)
    //      1: texture_2d<f32>   (gbuffer normal)
    //      2: texture_storage_2d<rgba16float, write>  (half-res indirect output)
    //      3: uniform mat4x4    (inv_view_proj)
    //      4: uniform vec4      (probe_origin_spacing: xyz=origin, w=spacing)
    //      5: uniform vec4      (probe_counts: xyz=Nx,Ny,Nz, w=unused)
    //      6: storage array<vec3>  (irradianceBuffer from LumenDDGIPass)
    //      7: storage array<f32>   (ddgiDepthBuffer from LumenDDGIPass)
    {
        namespace rhi = primal::graphics::rhi;
        std::string shaderSrc = LoadShaderSource("Engine/Graphics/Dawn/shaders/Lumen/DDGIGIGather.wgsl");
        if (shaderSrc.empty()) {
            std::cerr << "[Mode10] Failed to load DDGIGIGather.wgsl — DDGI disabled" << std::endl;
            // Roll back prior allocations (mirror Task 9's cleanup pattern)
            device_->DestroyTexture(giIndirectTexture_);  giIndirectTexture_ = rhi::handles::INVALID_RESOURCE;
            device_->DestroyTexture(prevHdrTexture_);     prevHdrTexture_ = rhi::handles::INVALID_RESOURCE;
            ddgiPass_.reset();
            staticProbeVolume_.reset();
            return;
        }

        auto cs = device_->CreateShader(shaderSrc.data(), shaderSrc.size(),
                                        rhi::ShaderStage::Compute, "ddgi_gi_gather");
        if (cs == rhi::handles::INVALID_SHADER) {
            std::cerr << "[Mode10] DDGIGIGather shader compile failed — DDGI disabled" << std::endl;
            device_->DestroyTexture(giIndirectTexture_);  giIndirectTexture_ = rhi::handles::INVALID_RESOURCE;
            device_->DestroyTexture(prevHdrTexture_);     prevHdrTexture_ = rhi::handles::INVALID_RESOURCE;
            ddgiPass_.reset();
            staticProbeVolume_.reset();
            return;
        }

        // DSL: 8 bindings — 3 textures + 5 buffers
        rhi::DescriptorSetLayoutBinding giGatherBindings[8] = {
            {0, rhi::DescriptorType::SampledDepthImage,  1, rhi::ShaderStage::Compute, nullptr}, // depth (texture_depth_2d in WGSL)
            {1, rhi::DescriptorType::SampledImage,  1, rhi::ShaderStage::Compute, nullptr}, // normal
            {2, rhi::DescriptorType::StorageImage,  1, rhi::ShaderStage::Compute, nullptr}, // output
            {3, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Compute, nullptr}, // invVP
            {4, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Compute, nullptr}, // origin/spacing
            {5, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Compute, nullptr}, // counts
            {6, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr}, // irradiance
            {7, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute, nullptr}, // depth
        };
        rhi::DescriptorSetLayoutDesc dslDesc{8, giGatherBindings};
        giGatherDsl_ = device_->CreateDescriptorSetLayout(dslDesc);
        if (giGatherDsl_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            std::cerr << "[Mode10] GIGather DSL creation failed — DDGI disabled" << std::endl;
            device_->DestroyTexture(giIndirectTexture_);  giIndirectTexture_ = rhi::handles::INVALID_RESOURCE;
            device_->DestroyTexture(prevHdrTexture_);     prevHdrTexture_ = rhi::handles::INVALID_RESOURCE;
            ddgiPass_.reset();
            staticProbeVolume_.reset();
            return;
        }

        rhi::PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &giGatherDsl_;
        giGatherPipelineLayout_ = device_->CreatePipelineLayout(plDesc);
        if (giGatherPipelineLayout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) {
            std::cerr << "[Mode10] GIGather pipeline layout creation failed — DDGI disabled" << std::endl;
            device_->DestroyDescriptorSetLayout(giGatherDsl_);
            giGatherDsl_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
            device_->DestroyTexture(giIndirectTexture_);  giIndirectTexture_ = rhi::handles::INVALID_RESOURCE;
            device_->DestroyTexture(prevHdrTexture_);     prevHdrTexture_ = rhi::handles::INVALID_RESOURCE;
            ddgiPass_.reset();
            staticProbeVolume_.reset();
            return;
        }

        rhi::ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = cs;
        pipeDesc.layout = giGatherPipelineLayout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        giGatherPipeline_ = device_->CreateComputePipeline(pipeDesc);
        if (giGatherPipeline_ == rhi::handles::INVALID_PIPELINE) {
            std::cerr << "[Mode10] GIGather pipeline creation failed — DDGI disabled" << std::endl;
            device_->DestroyPipelineLayout(giGatherPipelineLayout_);
            giGatherPipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
            device_->DestroyDescriptorSetLayout(giGatherDsl_);
            giGatherDsl_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
            device_->DestroyTexture(giIndirectTexture_);  giIndirectTexture_ = rhi::handles::INVALID_RESOURCE;
            device_->DestroyTexture(prevHdrTexture_);     prevHdrTexture_ = rhi::handles::INVALID_RESOURCE;
            ddgiPass_.reset();
            staticProbeVolume_.reset();
            return;
        }

        rhi::DescriptorSetDesc dsDesc;
        dsDesc.layout = giGatherDsl_;
        giGatherDescriptorSet_ = device_->CreateDescriptorSet(dsDesc);

        // 3-frame rotating Constant Buffers. Each packs GIGatherCB (96 bytes,
        // matches TestNaniteStreamingPipeline.cpp:3734 layout):
        //   offset 0:  m4x4 inv_view_projection  (64 bytes)
        //   offset 64: v4 probe_origin_spacing   (16 bytes)
        //   offset 80: v4 probe_counts           (16 bytes)
        for (int i = 0; i < 3; ++i) {
            rhi::BufferDesc cbDesc{};
            cbDesc.size = 256;  // round up to 256 for CB alignment
            cbDesc.type = rhi::BufferType::Constant;
            cbDesc.usage = rhi::GPUMemoryUsage::Dynamic;
            cbDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
            giGatherCbBuffers_[i] = device_->CreateBuffer(cbDesc);
            if (giGatherCbBuffers_[i] == rhi::handles::INVALID_RESOURCE) {
                std::cerr << "[Mode10] GIGather CB[" << i << "] creation failed — DDGI disabled" << std::endl;
                // Roll back all prior CBs + pipeline + DSL + textures + pass
                for (int j = 0; j < i; ++j) {
                    device_->DestroyBuffer(giGatherCbBuffers_[j]);
                    giGatherCbBuffers_[j] = rhi::handles::INVALID_RESOURCE;
                }
                device_->DestroyDescriptorSet(giGatherDescriptorSet_);
                giGatherDescriptorSet_ = rhi::handles::INVALID_DESCRIPTOR_SET;
                device_->DestroyPipeline(giGatherPipeline_);
                giGatherPipeline_ = rhi::handles::INVALID_PIPELINE;
                device_->DestroyPipelineLayout(giGatherPipelineLayout_);
                giGatherPipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
                device_->DestroyDescriptorSetLayout(giGatherDsl_);
                giGatherDsl_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
                device_->DestroyTexture(giIndirectTexture_);
                giIndirectTexture_ = rhi::handles::INVALID_RESOURCE;
                device_->DestroyTexture(prevHdrTexture_);
                prevHdrTexture_ = rhi::handles::INVALID_RESOURCE;
                ddgiPass_.reset();
                staticProbeVolume_.reset();
                return;
            }
        }

        std::cout << "[Mode10] GIGather pipeline initialized (8x8 workgroups)" << std::endl;
    }

    ddgiEnabled_ = true;
    std::cout << "[Mode10] DDGI initialized successfully" << std::endl;
}

void Engine_Test::ShutdownDDGIForMode10() {
    if (!ddgiEnabled_ && !ddgiPass_ && !staticProbeVolume_ &&
        giGatherPipeline_ == rhi::handles::INVALID_PIPELINE) return;

    // Release GIGather pipeline + CBs (reverse order of init)
    for (int i = 0; i < 3; ++i) {
        if (giGatherCbBuffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(giGatherCbBuffers_[i]);
            giGatherCbBuffers_[i] = rhi::handles::INVALID_RESOURCE;
        }
    }
    if (giGatherDescriptorSet_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        device_->DestroyDescriptorSet(giGatherDescriptorSet_);
        giGatherDescriptorSet_ = rhi::handles::INVALID_DESCRIPTOR_SET;
    }
    if (giGatherPipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(giGatherPipeline_);
        giGatherPipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    if (giGatherPipelineLayout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(giGatherPipelineLayout_);
        giGatherPipelineLayout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }
    if (giGatherDsl_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(giGatherDsl_);
        giGatherDsl_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }

    // (ddgiGraph_ has no persistent GPU resources — Clear/Execute per frame,
    //  so a plain reset() is sufficient before ddgiPass_->Shutdown tears down
    //  the actual probe storage.)
    ddgiGraph_.reset();

    if (ddgiPass_) {
        ddgiPass_->Shutdown();
        ddgiPass_.reset();
    }
    if (staticProbeVolume_) {
        staticProbeVolume_->Shutdown();
        staticProbeVolume_.reset();
    }
    if (giIndirectTexture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(giIndirectTexture_);
        giIndirectTexture_ = rhi::handles::INVALID_RESOURCE;
    }
    if (prevHdrTexture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(prevHdrTexture_);
        prevHdrTexture_ = rhi::handles::INVALID_RESOURCE;
    }

    ddgiEnabled_ = false;
    std::cout << "[Mode10] DDGI shut down" << std::endl;
}

void Engine_Test::RenderMeshletDDGIFrame(primal::graphics::rhi::RHICommandBuffer* cmd) {
    // Mode 10 = Mode 9 meshlet path + DDGI indirect lighting.
    // Task 11a stub: lazy-init DDGI on first Mode 10 entry, then run plain
    // Mode 9 meshlet path. Task 11b will add the per-frame DDGI pass + GIGather
    // dispatch + prev-HDR blit.

    if (!ddgiEnabled_) {
        InitializeDDGIForMode10();
        // If init failed, ddgiEnabled_ stays false; RenderMeshletFrame below
        // won't apply DDGI (applyDDGI gate in RenderDawnMeshletDeferredLighting
        // call site falls through). Mode 10 effectively degrades to Mode 9.
    }

    // Run the standard meshlet path. The DDGI enable flag + giIndirectTexture_
    // are read inside RenderMeshletFrame via the call-site plumbing
    // (see "applyDDGI" in the RenderDawnMeshletDeferredLighting call).
    RenderMeshletFrame(cmd);

    // --- End-of-frame prev-HDR blit ---
    // Copy current HDR color into prevHdrTexture_ for NEXT frame's DDGI trace
    // radiance feedback. Skipped on frame 0 (no prior HDR content worth feeding
    // back) and when DDGI is disabled or prevHdrTexture_ wasn't allocated.
    if (ddgiEnabled_ && frameIndex_ > 0 &&
        prevHdrTexture_ != primal::graphics::rhi::handles::INVALID_RESOURCE &&
        hdrTexture_     != primal::graphics::rhi::handles::INVALID_RESOURCE) {
        namespace rhi = primal::graphics::rhi;

        // 1. Barrier hdrTexture_: RenderTarget → ShaderResource (blit source)
        rhi::ResourceBarrier hdrToSRV{};
        hdrToSRV.resource      = hdrTexture_;
        hdrToSRV.beforeState   = rhi::ResourceState::RenderTarget;
        hdrToSRV.afterState    = rhi::ResourceState::ShaderResource;
        hdrToSRV.subresource   = 0;
        hdrToSRV.queueFamily   = 0;
        cmd->InsertBarrier(&hdrToSRV, 1);

        // 2. Blit (Nearest — same dimensions, no filtering needed)
        rhi::TextureBlitRegion region{};
        region.srcSubresource  = {0, 0, 1};
        region.srcOffsets[0]   = {0, 0, 0};
        region.srcOffsets[1]   = {(s32)width_, (s32)height_, 1};
        region.dstSubresource  = {0, 0, 1};
        region.dstOffsets[0]   = {0, 0, 0};
        region.dstOffsets[1]   = {(s32)width_, (s32)height_, 1};
        cmd->BlitTexture(hdrTexture_, prevHdrTexture_, &region, 1, rhi::FilterMode::Nearest);

        // 3. Barrier hdrTexture_: ShaderResource → RenderTarget (restore for next frame)
        rhi::ResourceBarrier hdrToRT{};
        hdrToRT.resource       = hdrTexture_;
        hdrToRT.beforeState    = rhi::ResourceState::ShaderResource;
        hdrToRT.afterState     = rhi::ResourceState::RenderTarget;
        hdrToRT.subresource    = 0;
        hdrToRT.queueFamily    = 0;
        cmd->InsertBarrier(&hdrToRT, 1);

        // 4. Explicitly place prevHdrTexture_ in ShaderResource state — RG will
        //    import it correctly next frame when LumenDDGIPass declares a Read.
        //    BlitTexture (Dawn) is a pure encoder-level CopyTextureToTexture; it
        //    does NOT update the RHI state tracker. prevHdrTexture_ was created
        //    with hdrDesc_ and its tracked state is still the post-create Ready
        //    (DawnTexture::Initialize sets Ready). Transitioning Ready → SR here
        //    gives RG a definite physical state at frame N+1 import.
        rhi::ResourceBarrier prevHdrToSR{};
        prevHdrToSR.resource      = prevHdrTexture_;
        prevHdrToSR.beforeState   = rhi::ResourceState::Ready;
        prevHdrToSR.afterState    = rhi::ResourceState::ShaderResource;
        prevHdrToSR.subresource   = 0;
        prevHdrToSR.queueFamily   = 0;
        cmd->InsertBarrier(&prevHdrToSR, 1);
    }
}

void Engine_Test::RenderMeshletFrame(primal::graphics::rhi::RHICommandBuffer* cmd) {
    if (!meshletInitialized_) {
        if (!InitializeMeshletPipeline()) {
            std::cerr << "[Meshlet] Init failed, falling back to NoEffects" << std::endl;
            renderMode_ = DawnRenderMode::NoEffects;
            return;
        }
    }

    auto& gpuDrawPipeline = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
    auto& cullingPipeline = primal::graphics::nanite::GPUCullingPipeline::Get();
    const u32 fi = frameIndex_ % kFrameCount;

    // Sync CPU→GPU scene buffers (instance/cluster refs). No-op after first frame
    // unless scene changes.
    meshletSceneSnapshot_.UploadToGPUBuffers(cmd);

    // --- 1. Shadow cascade pass (2 cascades, matches ShadowFrameResources[2]) ---
    ComputeCSMViewProjections();
    for (u32 cascade = 0; cascade < 2; ++cascade) {
        primal::graphics::nanite::GPUDrivenDrawPipeline::DirectionalLightData lightData{};
        // light dir from scene
        const auto& lights = scene_.GetLights();
        if (!lights.empty()) {
            lightData.direction = primal::math::v4{lights[0].direction.x, lights[0].direction.y, lights[0].direction.z, 0.0f};
            lightData.color = primal::math::v4{lights[0].color.x, lights[0].color.y, lights[0].color.z, lights[0].intensity};
        }
        lightData.viewPos = primal::math::v4{cameraPos_.x, cameraPos_.y, cameraPos_.z, 1.0f};
        lightData.shadowMatrix0 = cascadeVPs_[0];
        lightData.shadowMatrix1 = cascadeVPs_[1];
        lightData.cascadeSplits = primal::math::v4{10.0f, 25.0f, 0.0f, 0.0f};

        gpuDrawPipeline.ExecuteShadowCulling(cmd, meshletSceneSnapshot_, lightData, cascade, fi);
        gpuDrawPipeline.ExecuteShadowRaster(cmd, cascadeVPs_[cascade], cascade, fi);
        gpuDrawPipeline.ExecuteShadowDepthBlit(cmd, cascade, fi);
    }

    // --- 2. Cull (HZB disabled for now — uses prev-frame depth which we don't have
    //     in this path; the culling kernel guards hzb_system_ null-ness).
    cullingPipeline.Execute(cmd, meshletSceneSnapshot_,
                            view_.GetViewMatrix(), view_.GetProjectionMatrix(),
                            nullptr, fi);
    const auto& cullingResults = cullingPipeline.GetResults();

    // --- 3. Meshlet draw — produces 4-RT GBuffer + final_depth_texture_.
    // Push the debug mode (V key state) before Execute so the GBuffer pass
    // can hash-color by meshlet_id / triangle_id / mesh_id when active.
    gpuDrawPipeline.SetDebugMode(meshletDebugMode_);
    if (!gpuDrawPipeline.Execute(cmd, meshletSceneSnapshot_,
                                 view_.GetViewMatrix(), view_.GetProjectionMatrix(),
                                 cullingResults, frameIndex_, fi)) {
        std::cerr << "[Meshlet] GPUDrivenDrawPipeline::Execute failed" << std::endl;
    }

    // --- 3b. DDGI dispatch (Mode 10 only) ---
    // Runs AFTER meshlet draw (GBuffer + depth ready) and BEFORE deferred
    // lighting (which samples giIndirectTexture_ at binding 13).
    //
    // Two sub-passes are registered on a SEPARATE RenderGraph (ddgiGraph_):
    //   (a) LumenDDGIPass::AddPass — TraceRays → UpdateIrradiance → UpdateDepth
    //   (b) Inline GIGather compute pass — reads DDGI probe storage + GBuffer,
    //       writes half-res giIndirectTexture_.
    //
    // The separate RG is required because the main renderGraph_ Executes at
    // line ~1103 (after deferred lighting). Using it would delay DDGI past
    // its consumer. ddgiGraph_ is cleared/rebuilt/compiled/executed per frame.
    //
    // Skip on frame 0: prevHdrTexture_ has no useful content yet, and the
    // per-frame CBs haven't been populated.
    if (renderMode_ == DawnRenderMode::MeshletSSGISSRDDGI &&
        ddgiEnabled_ && ddgiPass_ && ddgiGraph_ &&
        giGatherPipeline_ != primal::graphics::rhi::handles::INVALID_PIPELINE &&
        giIndirectTexture_ != primal::graphics::rhi::handles::INVALID_RESOURCE &&
        prevHdrTexture_    != primal::graphics::rhi::handles::INVALID_RESOURCE &&
        frameIndex_ > 0) {

        namespace rhi = primal::graphics::rhi;
        namespace rg  = primal::graphics::rendergraph;
        using namespace primal::graphics::lumen;

        // 1. Fresh DDGI graph for this frame
        ddgiGraph_->Clear();

        // 2. Import prev-HDR + half-res indirect output textures.
        //    prevHdrTexture_ format/dimensions match hdrDesc_ (allocated from it).
        auto prevHdrRG = ddgiGraph_->ImportTexture("DDGI_PrevHDR", prevHdrTexture_, hdrDesc_);
        rhi::TextureDesc giDesc{};
        giDesc.size   = {width_ / 2, height_ / 2, 1u};
        giDesc.format = rhi::DataFormat::RGBA16_Float;
        giDesc.type   = rhi::TextureType::Texture2D;
        giDesc.usage  = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
        auto giIndirectRG = ddgiGraph_->ImportTexture("DDGI_GIIndirect", giIndirectTexture_, giDesc);

        // 3. Camera data (Phase A: prev = current — no TAA history yet)
        DDGICameraData camData{};
        camData.camera_position   = cameraPos_;
        camData.view_matrix       = view_.GetViewMatrix();
        camData.proj_matrix       = view_.GetProjectionMatrix();
        camData.prev_view_matrix  = camData.view_matrix;
        camData.prev_proj_matrix  = camData.proj_matrix;
        camData.light_direction   = simd::normalize(primal::math::v3{-0.5f, -1.0f, -0.3f});
        camData.light_color       = primal::math::v3{2.5f, 2.4f, 2.1f};
        camData.frame_index       = frameIndex_;
        camData.delta_time        = 1.0f / 60.0f;

        // 4. Register DDGI TraceRays → UpdateIrradiance → UpdateDepth
        //    on the same (separate) graph. ddgiOut gives the irradiance/depth
        //    RG handles that we read below in GIGather.
        auto ddgiOut = ddgiPass_->AddPass(*ddgiGraph_, prevHdrRG, camData, frameIndex_);

        // 5. Inline GIGather pass — reads DDGI output (irradiance_hist RG handle)
        //    + GBuffer textures, writes giIndirectRG.
        struct GIGatherData {};
        ddgiGraph_->AddPass<GIGatherData>("GIGather",
            rg::RGPassType::Compute,
            rg::RGPassCategory::Lighting,
            [giIndirectRG, ddgiIrrHist = ddgiOut.ddgi_irradiance_hist](GIGatherData&,
                                                                       rg::RenderGraphBuilder& builder) {
                builder.Write(giIndirectRG, rhi::ResourceState::UnorderedAccess);
                // DDGI storage-buffer read dependency — the RG inserts a UA→SR
                // barrier between LumenDDGIPass's write and our read.
                if (ddgiIrrHist.IsValid()) {
                    builder.Read(ddgiIrrHist, rhi::ResourceState::ShaderResource);
                }
            },
            [this, frameIdx = frameIndex_](const GIGatherData&,
                                           rg::RenderGraphContext& context) {
                auto cmd = context.cmdBuffer;
                if (!cmd) return;

                auto& gpuDraw = primal::graphics::nanite::GPUDrivenDrawPipeline::Get();

                // Read buffer triple-buffering: pick the slot DDGI just wrote this frame.
                // Per LumenDDGIPass, irradiance_buffers_[frame_idx % 3] is the freshest.
                // Canonical pattern (TestNaniteStreamingPipeline.cpp:3681) reads
                // (currentBufferIndex + 2) % 3 for the safer "previous-frame" view;
                // we mirror that.
                u32 fi = frameIdx % kFrameCount;
                u32 ddgiReadIdx = (fi + 2u) % 3u;
                rhi::ResourceHandle irradianceBuf = ddgiPass_->GetIrradianceBuffer(ddgiReadIdx);
                rhi::ResourceHandle depthBuf      = ddgiPass_->GetDepthBuffer(ddgiReadIdx);
                rhi::ResourceHandle depthTex      = gpuDraw.GetGBufferDepthSampleable();
                rhi::ResourceHandle normalTex     = gpuDraw.GetGBufferNormal();

                if (irradianceBuf == rhi::handles::INVALID_RESOURCE ||
                    depthBuf      == rhi::handles::INVALID_RESOURCE ||
                    depthTex      == rhi::handles::INVALID_RESOURCE ||
                    normalTex     == rhi::handles::INVALID_RESOURCE) {
                    return;
                }

                // --- Update GIGather CB (96 bytes packed at offsets 0/64/80) ---
                struct GIGatherCB {
                    primal::math::m4x4 inv_view_projection;  // offset 0, 64 bytes
                    primal::math::v4   probe_origin_spacing; // offset 64, 16 bytes
                    primal::math::v4   probe_counts;         // offset 80, 16 bytes
                };
                GIGatherCB cbData{};
                primal::math::m4x4 viewProj = view_.GetProjectionMatrix() * view_.GetViewMatrix();
                cbData.inv_view_projection = primal::graphics::rhi::math::Inverse(viewProj);
                const auto& volData = ddgiPass_->GetVolumeData();
                const auto& params  = ddgiPass_->GetParams();
                cbData.probe_origin_spacing = primal::math::v4{
                    volData.ProbeOrigin.x, volData.ProbeOrigin.y,
                    volData.ProbeOrigin.z, params.probe_spacing};
                cbData.probe_counts = primal::math::v4{
                    static_cast<f32>(params.probe_count_x),
                    static_cast<f32>(params.probe_count_y),
                    static_cast<f32>(params.probe_count_z), 0.0f};

                // Map/Unmap pattern (Dawn Storage/Uniform buffers are GPU-owned).
                // CB size is 256 bytes (allocated at InitializeDDGIForMode10).
                if (void* mapped = device_->MapBuffer(giGatherCbBuffers_[fi], 0, sizeof(cbData))) {
                    std::memcpy(mapped, &cbData, sizeof(cbData));
                    device_->UnmapBuffer(giGatherCbBuffers_[fi]);
                }

                // --- Write descriptor set: 3 textures + 5 buffer bindings ---
                rhi::DescriptorImageInfo imgInfos[3]{};
                imgInfos[0].imageView   = depthTex;
                imgInfos[0].imageLayout = rhi::ResourceState::ShaderResource;
                imgInfos[1].imageView   = normalTex;
                imgInfos[1].imageLayout = rhi::ResourceState::ShaderResource;
                imgInfos[2].imageView   = giIndirectTexture_;
                imgInfos[2].imageLayout = rhi::ResourceState::UnorderedAccess;

                rhi::DescriptorBufferInfo bufInfos[5]{};
                // bindings 3/4/5 all point at the SAME 256-byte CB, at offsets 0/64/80
                bufInfos[0].buffer = giGatherCbBuffers_[fi];
                bufInfos[0].offset = 0;
                bufInfos[0].range  = 64;
                bufInfos[1].buffer = giGatherCbBuffers_[fi];
                bufInfos[1].offset = 64;
                bufInfos[1].range  = 16;
                bufInfos[2].buffer = giGatherCbBuffers_[fi];
                bufInfos[2].offset = 80;
                bufInfos[2].range  = 16;
                bufInfos[3].buffer = irradianceBuf;
                bufInfos[3].offset = 0;
                bufInfos[3].range  = ~0ull;
                bufInfos[4].buffer = depthBuf;
                bufInfos[4].offset = 0;
                bufInfos[4].range  = ~0ull;

                rhi::WriteDescriptorSet writes[8]{};
                // Textures
                writes[0].dstSet          = giGatherDescriptorSet_;
                writes[0].dstBinding      = 0;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType  = rhi::DescriptorType::SampledDepthImage; // texture_depth_2d
                writes[0].imageInfo       = &imgInfos[0];
                writes[1].dstSet          = giGatherDescriptorSet_;
                writes[1].dstBinding      = 1;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType  = rhi::DescriptorType::SampledImage;
                writes[1].imageInfo       = &imgInfos[1];
                writes[2].dstSet          = giGatherDescriptorSet_;
                writes[2].dstBinding      = 2;
                writes[2].descriptorCount = 1;
                writes[2].descriptorType  = rhi::DescriptorType::StorageImage;
                writes[2].imageInfo       = &imgInfos[2];
                // CB bindings 3/4/5
                for (int i = 0; i < 3; ++i) {
                    writes[3 + i].dstSet          = giGatherDescriptorSet_;
                    writes[3 + i].dstBinding      = static_cast<u32>(3 + i);
                    writes[3 + i].descriptorCount = 1;
                    writes[3 + i].descriptorType  = rhi::DescriptorType::UniformBuffer;
                    writes[3 + i].bufferInfo      = &bufInfos[i];
                }
                // Storage buffers 6/7
                writes[6].dstSet          = giGatherDescriptorSet_;
                writes[6].dstBinding      = 6;
                writes[6].descriptorCount = 1;
                writes[6].descriptorType  = rhi::DescriptorType::StorageBuffer;
                writes[6].bufferInfo      = &bufInfos[3];
                writes[7].dstSet          = giGatherDescriptorSet_;
                writes[7].dstBinding      = 7;
                writes[7].descriptorCount = 1;
                writes[7].descriptorType  = rhi::DescriptorType::StorageBuffer;
                writes[7].bufferInfo      = &bufInfos[4];

                device_->UpdateDescriptorSets(8, writes);

                // Note: No explicit UA→SR barrier on irradianceBuf/depthBuf here.
                // The RG already inserts it via the builder.Read(ddgiIrrHist, ...)
                // declaration in the setup lambda above. Inserting a duplicate
                // barrier here causes validation errors on stricter backends and
                // is a no-op on Dawn (WebGPU handles sync internally).

                // --- Bind + dispatch ---
                cmd->BindComputePipeline(giGatherPipeline_);
                const rhi::DescriptorSetHandle sets[] = { giGatherDescriptorSet_ };
                cmd->BindDescriptorSets(rhi::PipelineBindPoint::Compute,
                                        giGatherPipelineLayout_, 0, 1, sets, 0, nullptr);

                u32 groupsX = (width_  / 2 + 7) / 8;
                u32 groupsY = (height_ / 2 + 7) / 8;
                cmd->Dispatch(groupsX, groupsY, 1);
            });

        // 6. Compile + Execute DDGI graph
        ddgiGraph_->Compile();
        ddgiGraph_->Execute(cmd);

        // 7. Barrier: giIndirectTexture_ UnorderedAccess → ShaderResource
        //    (DeferredLighting reads it via textureSampleLevel at binding 13.)
        rhi::ResourceBarrier giBarrier{};
        giBarrier.resource     = giIndirectTexture_;
        giBarrier.beforeState  = rhi::ResourceState::UnorderedAccess;
        giBarrier.afterState   = rhi::ResourceState::ShaderResource;
        giBarrier.subresource  = 0;
        giBarrier.queueFamily  = 0;
        cmd->InsertBarrier(&giBarrier, 1);
    }

    // --- 4. Wire the meshlet GBuffer + sampleable depth into deferred lighting.
    //     final_depth_texture_ is D32_Float with ShaderResource usage (set at
    //     creation in GPUDrivenDrawPipeline.cpp:440). No barrier needed — Dawn
    //     barriers are no-ops; state tracked for downstream consumers only.
    rhi::ResourceHandle meshletGBuffer[4] = {
        gpuDrawPipeline.GetGBufferAlbedo(),
        gpuDrawPipeline.GetGBufferNormal(),
        gpuDrawPipeline.GetGBufferORM(),
        gpuDrawPipeline.GetGBufferVelocity(),
    };
    rhi::ResourceHandle meshletDepth = gpuDrawPipeline.GetFinalDepthTexture();

    // Point shadow + IBL at the existing renderer-owned bindings so the meshlet
    // deferred shader sees the same shadow map / cube maps as the other modes.
    // SetDawnShadowResources was already called during init.
    // enableIBL: Mode 7 (MeshletNoIBL) skips the IBL ambient term; Mode 8/9 apply it.
    forwardRenderer_.SetDawnEnableIBL((renderMode_ == DawnRenderMode::Meshlet ||
                                       renderMode_ == DawnRenderMode::MeshletSSGISSR ||
                                       renderMode_ == DawnRenderMode::MeshletSSGISSRDDGI) ? 1u : 0u);
    // Mode 10 (MeshletSSGISSRDDGI) layers DDGI indirect on top of Mode 9's meshlet
    // path. enableDDGI gates the WGSL DDGI block in DeferredLighting_Meshlet.wgsl;
    // giIndirectTexture_ feeds binding 13. When DDGI isn't initialized or mode !=
    // 10, giIndirectTexture_ is INVALID_RESOURCE and ForwardRenderer falls back
    // to its 1x1 dummy texture (no-op).
    const bool applyDDGI = (renderMode_ == DawnRenderMode::MeshletSSGISSRDDGI) && ddgiEnabled_;
    forwardRenderer_.SetDawnEnableDDGI(applyDDGI ? 1u : 0u);
    forwardRenderer_.RenderDawnMeshletDeferredLighting(
        cmd, view_, meshletGBuffer, meshletDepth, hdrTexture_, scene_,
        frameIndex_, width_, height_,
        applyDDGI ? giIndirectTexture_ : primal::graphics::rhi::handles::INVALID_RESOURCE);

    // Blit meshlet velocity (RG16_Float) → velocityTexture_ so downstream TAA
    // sees per-pixel motion vectors. RG16_Float on both sides
    // (GPUDrivenDrawPipeline.cpp:446, TestDawnForwardRenderer.cpp:236) — formats match.
    rhi::TextureBlitRegion velRegion{};
    velRegion.srcSubresource = {0, 0, 1};
    velRegion.srcOffsets[0] = {0, 0, 0};
    velRegion.srcOffsets[1] = {(s32)width_, (s32)height_, 1};
    velRegion.dstSubresource = {0, 0, 1};
    velRegion.dstOffsets[0] = {0, 0, 0};
    velRegion.dstOffsets[1] = {(s32)width_, (s32)height_, 1};
    cmd->BlitTexture(gpuDrawPipeline.GetGBufferVelocity(), velocityTexture_,
                     &velRegion, 1, rhi::FilterMode::Nearest);

    // Update previous view-projection for next frame's velocity calc. The
    // meshlet pipeline caches its own prev matrices internally; this update
    // only feeds the renderer-side TAA predictor and the meshlet deferred's
    // prevViewProjection uniform.
    forwardRenderer_.SetDawnShadowLightVP(cascadeVPs_[0]);
}

#ifdef __APPLE__
void Engine_Test::DisplayLinkCallback(CFRunLoopTimerRef, void* info) {
    auto* test = static_cast<Engine_Test*>(info);
    test->RenderFrame();
}
#endif

void Engine_Test::run() {
    timer_.begin();
    lastFrameTime_ = std::chrono::steady_clock::now();
}

void Engine_Test::shutdown() {
#ifndef __EMSCRIPTEN__
    ShutdownMeshletPipeline();
    ShutdownDDGIForMode10();
#endif
    forwardRenderer_.Shutdown();
    DestroyDepthTexture();
    DestroyPrepassDepthTexture();

    // Shadow resources
    if (shadowDepthTexture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(shadowDepthTexture_);
        shadowDepthTexture_ = rhi::handles::INVALID_RESOURCE;
    }

    // IBL resources
    if (iblIrradianceTex_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(iblIrradianceTex_);
        iblIrradianceTex_ = rhi::handles::INVALID_RESOURCE;
    }
    if (iblPrefilterTex_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(iblPrefilterTex_);
        iblPrefilterTex_ = rhi::handles::INVALID_RESOURCE;
    }
    if (iblBRDFLUTTex_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(iblBRDFLUTTex_);
        iblBRDFLUTTex_ = rhi::handles::INVALID_RESOURCE;
    }
    if (iblSampler_ != rhi::handles::INVALID_SAMPLER) {
        device_->DestroySampler(iblSampler_);
        iblSampler_ = rhi::handles::INVALID_SAMPLER;
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
    if (velocityTexture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(velocityTexture_);
        velocityTexture_ = rhi::handles::INVALID_RESOURCE;
    }
    for (u32 i = 0; i < kGBufferRTCount; ++i) {
        if (gbufferTextures_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyTexture(gbufferTextures_[i]);
            gbufferTextures_[i] = rhi::handles::INVALID_RESOURCE;
        }
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

    primal::jobsystem::JobSystem::Shutdown();
}

#ifdef __APPLE__
void Engine_Test::applicationDidFinishLaunching(NS::Notification* notification) {
    NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
    pApp->activateIgnoringOtherApps(true);

    if (!initialize()) {
        std::cerr << "[TestDawnFR] Init FAILED" << std::endl;
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
    _exit(0);
}
#endif

#endif // ENABLE_WEBGPU
