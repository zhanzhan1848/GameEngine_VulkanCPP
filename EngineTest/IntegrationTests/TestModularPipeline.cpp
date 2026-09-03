#include "TestModularPipeline.h"
#include "Engine/Common/CommonHeaders.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalMath.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/PCG/PCGSDFReadbackManager.h"  // HalfToFloat (GI verify)
#include "Engine/Utilities/IOStream.h"
#include "stb_image.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef TEST_METAL_GI_FUSION
// ============================================================================
// TestMetalGIFusion — GI full-composite alignment verification (Metal).
//
// Same StandardRenderPipeline / RHI code path as the Vulkan smoke test; the
// ONLY Metal-specific piece is the shader set (engine Metal Lumen shaders).
// After a warmup sequence it measures the GI chain quantitatively and checks
// alignment bands (recalibrated for the SC-first hierarchy — surface cache →
// world probes → screen probes; the old bands encoded the flat-blend look):
//   probe sh0 lum ∈ [0.08, 1.5]     (SC-projected energy; L0 clamp = 10)
//   gather variation max/avg ≥ 1.2  (smoother by design: 8-probe trilinear)
//   final dynamic range ≥ 0.1
//   final avg ∈ [0.005, 0.65]       (hierarchical fusion lifts brightness)
// ============================================================================
#define GI_VERIFY_FRAME 90
#endif

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
    uint16_t FloatToHalf(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
        const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
        const uint32_t mantissa = (bits >> 13) & 0x3ffu;
        if (exponent <= 0) return sign;
        if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
        return static_cast<uint16_t>(sign | (exponent << 10) | mantissa);
    }

    bool UploadTextureLayers(RHIDeviceBase* device, ResourceHandle texture,
                             const void* data, u64 bytesPerLayer,
                             u32 width, u32 height, u32 layerCount) {
        BufferDesc stagingDesc{};
        stagingDesc.size = bytesPerLayer * layerCount;
        stagingDesc.type = BufferType::Raw;
        stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        stagingDesc.name = "ModularPipelineTextureStaging";
        const ResourceHandle staging = device->CreateBuffer(stagingDesc);
        if (staging == handles::INVALID_RESOURCE ||
            !device->UpdateBufferData(staging, data, stagingDesc.size, 0)) {
            if (staging != handles::INVALID_RESOURCE) device->DestroyBuffer(staging);
            return false;
        }

        const CommandBufferHandle commandHandle =
            device->CreateCommandBuffer(CommandQueueType::Graphics);
        RHICommandBuffer* command = rhi::GetCommandBuffer(commandHandle);
        if (!command || !command->Begin()) {
            device->DestroyCommandBuffer(commandHandle);
            device->DestroyBuffer(staging);
            return false;
        }

        std::vector<BufferTextureCopyRegion> regions(layerCount);
        for (u32 layer = 0; layer < layerCount; ++layer) {
            auto& region = regions[layer];
            region.bufferOffset = bytesPerLayer * layer;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource = {0, layer, 1};
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {width, height, 1};
        }
        command->CopyBufferToTexture(staging, texture, regions.data(), layerCount);

        ResourceBarrier toShaderResource{};
        toShaderResource.resource = texture;
        toShaderResource.beforeState = ResourceState::CopyDest;
        toShaderResource.afterState = ResourceState::ShaderResource;
        toShaderResource.subresource = 0xffffffffu;
        toShaderResource.queueFamily = 0xffffffffu;
        command->InsertBarrier(&toShaderResource, 1);
        if (!command->End()) {
            device->DestroyCommandBuffer(commandHandle);
            device->DestroyBuffer(staging);
            return false;
        }

        const SyncHandle fence = device->CreateSync();
        QueueSubmitInfo submit{};
        submit.cmdBuffer = commandHandle;
        submit.signalFence = fence;
        const bool submitted = device->Submit(submit);
        const bool completed = submitted && device->WaitForSync(fence, UINT32_MAX);
        device->DestroySync(fence);
        device->DestroyCommandBuffer(commandHandle);
        device->DestroyBuffer(staging);
        return completed;
    }

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
    const shader_file_info kDeferredVS     { "DeferredLighting.metal", "vertexMain",                    ::shader_type::vertex };
    const shader_file_info kDeferredPS     { "DeferredLighting.metal", "fragmentLighting_gpuDriven",     ::shader_type::pixel  };
    const shader_file_info kBlitVS         { "DeferredLighting.metal", "vertexMain",                    ::shader_type::vertex };
    // T8.13 sync: FinalBlit samples the FUSION module's already-tone-mapped
    // LDR output (postToonColor / fusionOut) — the tonemapping fragmentBlit
    // applied a SECOND ACES+gamma pass on top and pinned the whole frame to
    // the white shoulder. fragmentBlitNoTonemap is the correct final blit
    // (same entry the Engine copy's mode-6 path uses).
    const shader_file_info kBlitPS         { "DeferredLighting.metal", "fragmentBlitNoTonemap",      ::shader_type::pixel  };
    const shader_file_info kShadowFilter   { "ShadowFilter.metal",     "shadow_filter_compute",          ::shader_type::compute };
    const shader_file_info kGIGather       { "DDGIGIGather.metal",     "ddgi_gi_gather",                 ::shader_type::compute };
    const shader_file_info kFusionIndirect { "DeferredLighting.metal", "fragmentFusionIndirect",         ::shader_type::pixel  };
    const shader_file_info kFusionComposite{ "DeferredLighting.metal", "fragmentFusion",                 ::shader_type::pixel  };
    const shader_file_info kSCCardRadiance { "Lumen/DDGICardRadianceAvg.metal",           "ddgi_card_radiance_avg",              ::shader_type::compute };
    const shader_file_info kSCProbeIrr     { "Lumen/DDGIProbeIrradianceFromCards.metal",  "ddgi_probe_irradiance_from_cards",    ::shader_type::compute };
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

    // Match the Vulkan reference's sunset HDR environment. Keep the test
    // usable on machines without the optional HDR asset or IBL shader support.
    if (!InitializeIBL()) {
        std::cerr << "[TestModularPipeline] IBL unavailable; using pipeline fallback ambient" << std::endl;
    }

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
    // Metal validation 恢复默认开（2026-09-02 二次修正）：后处理链启用后
    // 出现启动即楔死 GPU 驱动、连带 WindowServer 崩溃的问题——validation
    // 关闭时非法用法直通驱动无法定位，先恢复拦截抓证据。
    // 确认链路干净后可用 PRIMAL_METAL_VALIDATE=0 关闭以降负载。
    {
        const char* v = std::getenv("PRIMAL_METAL_VALIDATE");
        desc.enableDebug = (v == nullptr) || v[0] != '0';
    }

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
        // GI_DEBUG_STRIP=1: fusion indirect outputs each GI input in vertical
        // strips (ddgi/ssgi/spgi/full) for washout diagnosis. Temp knob.
        if (std::getenv("GI_DEBUG_STRIP"))
            extraArgs.push_back(L"-DGI_DEBUG_STRIP=1");
        if (std::getenv("DDGI_DBG_PATH"))
            extraArgs.push_back(L"-DDDGI_DBG_PATH=1");
        auto compiled = compile_shader(info, testShaderPath.c_str(), extraArgs);
        if (!compiled) {
            std::cerr << "[TestModularPipeline] Failed to compile " << info.file_name << ":" << info.function << std::endl;
            return handles::INVALID_SHADER;
        }

        u64 byteCodeSize = *reinterpret_cast<u64*>(compiled.get());
        const u8* byteCodePtr = compiled.get() + sizeof(u64) + 16;

        ShaderStage stage;
        switch (info.type) {
            case ::shader_type::vertex:    stage = ShaderStage::Vertex;   break;
            case ::shader_type::pixel:     stage = ShaderStage::Pixel;    break;
            case ::shader_type::compute:   stage = ShaderStage::Compute;  break;
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
#ifdef TEST_METAL_GI_FUSION
    // Compile the gather from the ENGINE Metal Lumen tree — this test exists
    // to validate that shader's alignment with the Vulkan gather path.
    ShaderHandle gg = handles::INVALID_SHADER;
    {
        const std::string engineLumenDir =
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/Lumen/";
        primal::utl::vector<std::wstring> noArgs;
        auto compiled = compile_shader(kGIGather, engineLumenDir.c_str(), noArgs);
        if (compiled) {
            u64 sz = *reinterpret_cast<u64*>(compiled.get());
            const u8* ptr = compiled.get() + sizeof(u64) + 16;
            gg = device_->CreateShader(ptr, sz, ShaderStage::Compute, kGIGather.function);
        }
    }
    check(gg, "gi_gather(engine Metal)");
#else
    auto gg = Get(kGIGather);         check(gg, "gi_gather");
#endif
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
    // T8.13 note: SetViewportSize triggers InitializeSubsystems() with the
    // pipeline DEFAULT quality (screen probes off, render_scale 1.0) — the
    // proven-stable configuration (0 GPU faults in 640+ frames). The lighting
    // calibration below flows through the per-frame CB upload, so it takes
    // effect without re-initialization.
    pipeline_->SetViewportSize(renderWidth_, renderHeight_);
    pipeline_->SetShaderHandles(shaderHandles_);

    // T8.13 sync: offline merged SDF — same wiring as TestVulkanSponzaRenderGraph.
    // First run merges (~5 min) and writes the cache; later runs load in ~1s.
    {
        const std::string sdfCache =
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza_global_sdf.v1.cache";
        const std::string pipeModelPath =
            "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Sponza_pipeline.model";
        offline_sdf_merger_ = new primal::graphics::nanite::OfflineSDFMerger();
        if (!offline_sdf_merger_->LoadFromCache(device_.get(), sdfCache.c_str())) {
            u32 sdfCount = offline_sdf_merger_->LoadFromPipelineModel(pipeModelPath.c_str());
            if (sdfCount > 0) {
                std::cerr << "[TestModularPipeline] Offline SDF loaded: " << sdfCount
                          << " meshes, merging..." << std::endl;
                if (!offline_sdf_merger_->BuildAndUpload(device_.get(), sdfCache.c_str())) {
                    std::cerr << "[TestModularPipeline] Offline SDF merge/upload failed" << std::endl;
                }
            } else {
                std::cerr << "[TestModularPipeline] No offline SDF data found" << std::endl;
            }
        }
        if (offline_sdf_merger_->IsValid()) {
            pipeline_->SetOfflineSDFSource(
                offline_sdf_merger_->GetTexture(),
                offline_sdf_merger_->GetOrigin(),
                offline_sdf_merger_->GetExtent(),
                offline_sdf_merger_->GetResolution());
            const auto so = offline_sdf_merger_->GetOrigin();
            const auto se = offline_sdf_merger_->GetExtent();
            const float maxExt = std::max({se.x, se.y, se.z});
            std::cerr << "[TestModularPipeline] Offline SDF wired: origin=("
                      << so.x << "," << so.y << "," << so.z << ") extent=("
                      << se.x << "," << se.y << "," << se.z << ") res="
                      << offline_sdf_merger_->GetResolution()
                      << " voxel=" << maxExt / offline_sdf_merger_->GetResolution()
                      << " traceThreshold=1.25x voxel=" << maxExt / offline_sdf_merger_->GetResolution()
                      << std::endl;
        } else {
            delete offline_sdf_merger_;
            offline_sdf_merger_ = nullptr;
        }
    }

    // T8.13 stability revert 确认仍然有效（2026-09-04 重测：High 档
    // 在 Metal 上出现洋红画面 = GPU 页故障，与历史记录一致）。
    // Medium + capture_budget=0（全量更新）是已验证的稳定基线，
    // GI 质量（弹射/亮度/对比度）已与 Vulkan 对齐。
    lumen::LumenConfig lumenConfig;
    lumenConfig.quality = lumen::LumenQualityPreset::Medium;
    lumenConfig.surface_cache_capture_budget = 0;
    // 2026-09-04 闪烁修复：temporal weight 0.02→0.005——增加时域平滑。
    // SC 全量更新后每帧 atlas 微变 → 探针 SH 微变 → 屏幕高频闪烁。
    // 降低 alpha 使探针更"黏"历史值，消除高频抖动。
    lumenConfig.ddgi_irradiance_temporal_weight = 0.02f;
    // 探针间距对齐 Medium 档预设 2.0（LumenConfig 默认 3.0 格边界在墙面
    // 可见为方块接缝；2.0 减少 33% 格尺寸，棋盘格缝明显减轻）。
    lumenConfig.ddgi_probe_spacing = 2.0f;
    // 2026-09-04 启用 SPGI（Screen Probe GI）——Vulkan High 档混合方案
    // = SPGI（主力高频 GI）+ DDGI（低频环境）+ SSGI。Metal 之前缺 SPGI
    // → GI 只有 DDGI 低频 → "很平"无彩色弹射细节。
    // SPGI Metal 实测全黑（shader WIP），暂不启用。
    // use_screen_probes=true 待 Metal SPGI shader 审计完成后再开。
    // use_screen_probes = true;
    pipeline_->SetLumenConfig(lumenConfig);

    // T8.13 Vulkan↔Metal sync — SAME lighting calibration as
    // TestVulkanSponzaRenderGraph (the measurement reference). Without this
    // the pipeline defaults (light_color 5.0, ddgi_light_color 20.0) put the
    // probe analytic drive at ~60x the Vulkan reference and pinned every
    // GI-lit surface to the white ACES shoulder.
    // 2026-09-02 再对齐：跟随 Vulkan 62c609bd 方向复原暖弹射，但 Metal 的
    // SC↔probe 环路增益高于 Vulkan（同 5.0 档 gather_avg=1.107 vs Vulkan
    // ~0.3-0.5，GIFusionVerify 上限 0.8），实测折中取 2.0：亮度回带内，
    // 弹射色保留（旧值 0.318 时探针场全中性 rgb(0.215,0.212,0.209)，
    // Vulkan 基准是暖色 (0.234,0.193,0.145)）。
    {
        auto renderSettings = pipeline_->GetSettings();
        renderSettings.lighting.light_direction = {0.5f, -0.7f, 0.3f};
        // PRIMAL_LIGHT_DIR="x,y,z"：GI 响应 A/B——翻转直射方向后对比 gather
        // 条带分布，验证 GI 场是否真的跟随直射光（"GI 区是平的"排查）。
        if (const char* ld = std::getenv("PRIMAL_LIGHT_DIR")) {
            float lx, ly, lz;
            if (std::sscanf(ld, "%f,%f,%f", &lx, &ly, &lz) == 3) {
                renderSettings.lighting.light_direction = {lx, ly, lz};
                std::cerr << "[TestModularPipeline] light_direction overridden: "
                          << lx << "," << ly << "," << lz << std::endl;
            }
        }
        renderSettings.lighting.light_color = {3.0f, 3.0f, 3.0f, 1.0f}; // 直射峰值对齐: 3.0 x albedo/pi 峰值 ~1.4 = Vulkan 实测 1.38 (7.5 档会到 ~3.6 死白)
        // 2026-09-04 对齐：SC 全量（capture_budget=0）+ moments CullMode::None
        // 修复后，环路结构已与 Vulkan 一致——ddgi_light_color 恢复 Vulkan
        // 同档 5.0（此前 2.0 是 SC 覆盖不足+诊断 stride 错误时期的折中值，
        // 导致探针场中性化、GI 无彩色弹射）。
        renderSettings.lighting.ddgi_light_color = {3.0f, 3.0f, 3.0f, 1.0f};
        pipeline_->UpdateSettings(renderSettings);
    }

    // T8.13 sync: match the Vulkan reference app's pass scope — volume paths
    // are explicitly out of scope there; fragmentFusion adds vol.rgb (fog
    // scatter) additively, so a Metal fog difference reads as a full-screen
    // white wash.
    pipeline_->SetPassEnabled(RenderPassID::SSR, false);
    pipeline_->SetPassEnabled(RenderPassID::VolumePass, false);
    pipeline_->SetPassEnabled(RenderPassID::VolumeRenderer, false);
    pipeline_->SetPassEnabled(RenderPassID::FroxelFog, false);
#ifndef TEST_METAL_GI_FUSION
    // Screen probes are enabled explicitly through the axis configuration
    // above; no late quality override is needed.
    // Froxel fog stays off — the Vulkan reference explicitly disables the
    // volume paths and fragmentFusion adds vol.rgb additively.
#endif

    // Load Sponza scene (populates scene_ and wires materials to GPUDrivenDrawPipeline)
    if (!LoadSponzaScene()) {
        std::cerr << "[TestModularPipeline] Failed to load Sponza scene!" << std::endl;
        return false;
    }

    return true;
}

void TestModularPipeline::SetupCamera() {
    float x = 0.0f, y = 5.0f, z = 0.0f, pitch = 0.0f, yaw = 0.0f;
    if (const char* cameraOverride = std::getenv("SPGI_CAM")) {
        if (std::sscanf(cameraOverride, "%f,%f,%f,%f,%f",
                        &x, &y, &z, &pitch, &yaw) == 5) {
            std::cout << "[TestModularPipeline] Camera override: " << cameraOverride << std::endl;
        }
    }
    camera_.Initialize({x, y, z}, {pitch, yaw, 0.0f});
    camera_.SetSpeed(10.0f, 0.1f);
}

bool TestModularPipeline::InitializeIBL() {
    const std::string hdrPath =
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/textures/hdr/sunset.hdr";
    int hdrWidth = 0;
    int hdrHeight = 0;
    int hdrChannels = 0;
    float* hdr = stbi_loadf(hdrPath.c_str(), &hdrWidth, &hdrHeight, &hdrChannels, 4);
    if (!hdr || hdrWidth < 4 || hdrHeight < 2) {
        if (hdr) stbi_image_free(hdr);
        std::cerr << "[TestModularPipeline] Failed to load HDR: " << hdrPath << std::endl;
        return false;
    }

    // Generate the same +X/-X/+Y/-Y/+Z/-Z cube convention used by the Metal
    // EquirectangularToCube shader. Doing this conversion on the CPU keeps this
    // integration test independent of an extra one-shot compute pipeline while
    // feeding IBLPrecomputer the same RGBA16F environment as Vulkan.
    const u32 faceSize = static_cast<u32>(hdrWidth / 4);
    const u64 pixelsPerFace = static_cast<u64>(faceSize) * faceSize;
    std::vector<uint16_t> cubePixels(pixelsPerFace * 6 * 4);
    constexpr float kPi = 3.14159265358979323846f;

    auto sampleHDR = [&](float u, float v, int channel) {
        u -= std::floor(u);
        v = std::clamp(v, 0.0f, 1.0f);
        const float fx = u * hdrWidth - 0.5f;
        const float fy = v * hdrHeight - 0.5f;
        const int x0Raw = static_cast<int>(std::floor(fx));
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, hdrHeight - 1);
        const int y1 = std::min(y0 + 1, hdrHeight - 1);
        const int x0 = (x0Raw % hdrWidth + hdrWidth) % hdrWidth;
        const int x1 = (x0 + 1) % hdrWidth;
        const float tx = fx - std::floor(fx);
        const float ty = fy - std::floor(fy);
        const float a = hdr[(static_cast<size_t>(y0) * hdrWidth + x0) * 4 + channel];
        const float b = hdr[(static_cast<size_t>(y0) * hdrWidth + x1) * 4 + channel];
        const float c = hdr[(static_cast<size_t>(y1) * hdrWidth + x0) * 4 + channel];
        const float d = hdr[(static_cast<size_t>(y1) * hdrWidth + x1) * 4 + channel];
        return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
    };

    for (u32 face = 0; face < 6; ++face) {
        for (u32 y = 0; y < faceSize; ++y) {
            for (u32 x = 0; x < faceSize; ++x) {
                const float faceU = 2.0f * (static_cast<float>(x) + 0.5f) / faceSize - 1.0f;
                const float faceV = -(2.0f * (static_cast<float>(y) + 0.5f) / faceSize - 1.0f);
                float dx = 0.0f, dy = 0.0f, dz = 0.0f;
                switch (face) {
                    case 0: dx =  1.0f; dy = faceV; dz = -faceU; break;
                    case 1: dx = -1.0f; dy = faceV; dz =  faceU; break;
                    case 2: dx = faceU; dy =  1.0f; dz = -faceV; break;
                    case 3: dx = faceU; dy = -1.0f; dz =  faceV; break;
                    case 4: dx = faceU; dy = faceV; dz =  1.0f; break;
                    default: dx = -faceU; dy = faceV; dz = -1.0f; break;
                }
                const float inverseLength = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);
                dx *= inverseLength;
                dy *= inverseLength;
                dz *= inverseLength;
                const float sphereU = std::atan2(dz, dx) / (2.0f * kPi) + 0.5f;
                const float sphereV = std::asin(dy) / kPi + 0.5f;
                const size_t destination =
                    (static_cast<size_t>(face) * pixelsPerFace +
                     static_cast<size_t>(y) * faceSize + x) * 4;
                for (int channel = 0; channel < 4; ++channel) {
                    cubePixels[destination + channel] =
                        FloatToHalf(sampleHDR(sphereU, sphereV, channel));
                }
            }
        }
    }
    stbi_image_free(hdr);

    TextureDesc cubeDesc{};
    cubeDesc.size = {faceSize, faceSize, 1};
    cubeDesc.arraySize = 1;
    cubeDesc.mipLevels = 1;
    cubeDesc.type = TextureType::TextureCube;
    cubeDesc.format = DataFormat::RGBA16_Float;
    cubeDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    cubeDesc.memoryUsage = GPUMemoryUsage::Static;
    cubeDesc.name = "IBL_EnvCube_Metal";
    envCube_ = device_->CreateTexture(cubeDesc);
    const u64 bytesPerFace = pixelsPerFace * 4 * sizeof(uint16_t);
    if (envCube_ == handles::INVALID_RESOURCE ||
        !UploadTextureLayers(device_.get(), envCube_, cubePixels.data(),
                             bytesPerFace, faceSize, faceSize, 6)) {
        if (envCube_ != handles::INVALID_RESOURCE) device_->DestroyTexture(envCube_);
        envCube_ = handles::INVALID_RESOURCE;
        return false;
    }

    iblPrecomputer_ = std::make_unique<IBLPrecomputer>(device_.get());
    if (!iblPrecomputer_->Initialize()) {
        iblPrecomputer_.reset();
        device_->DestroyTexture(envCube_);
        envCube_ = handles::INVALID_RESOURCE;
        return false;
    }

    iblIrradiance_ = iblPrecomputer_->ComputeIrradianceMap(envCube_, 32);
    iblPrefilter_ = iblPrecomputer_->ComputePrefilteredEnvironmentMap(envCube_, 128);
    iblBrdfLUT_ = iblPrecomputer_->ComputeBRDFIntegrationMap(512);
    if (iblIrradiance_ == handles::INVALID_RESOURCE ||
        iblPrefilter_ == handles::INVALID_RESOURCE ||
        iblBrdfLUT_ == handles::INVALID_RESOURCE) {
        auto destroy = [&](ResourceHandle& texture) {
            if (texture != handles::INVALID_RESOURCE) device_->DestroyTexture(texture);
            texture = handles::INVALID_RESOURCE;
        };
        destroy(iblIrradiance_);
        destroy(iblPrefilter_);
        destroy(iblBrdfLUT_);
        destroy(envCube_);
        iblPrecomputer_->Shutdown();
        iblPrecomputer_.reset();
        return false;
    }

    pipeline_->SetIBLResources(iblIrradiance_, iblPrefilter_, iblBrdfLUT_);
    std::cout << "[TestModularPipeline] Vulkan-reference sunset IBL wired" << std::endl;
    return true;
}

// ============================================================================
// Scene Loading
// ============================================================================

bool TestModularPipeline::LoadMaterialTextures() {
    std::string assetBaseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";

    // Texture arrays require equal dimensions. Match Vulkan's fallback
    // semantics while retaining this test's established 1024x1024 array size.
    ResourceHandle whiteTexture;
    ResourceHandle flatNormalTexture;
    ResourceHandle defaultORMTexture;
    {
        constexpr u32 WHITE_SIZE = 1024;
        std::vector<u32> whiteData(WHITE_SIZE * WHITE_SIZE, 0xFFFFFFFF);
        std::vector<u8> normalData(WHITE_SIZE * WHITE_SIZE * 4);
        std::vector<u8> ormData(WHITE_SIZE * WHITE_SIZE * 4);
        for (u32 pixel = 0; pixel < WHITE_SIZE * WHITE_SIZE; ++pixel) {
            normalData[pixel * 4 + 0] = 128;
            normalData[pixel * 4 + 1] = 128;
            normalData[pixel * 4 + 2] = 255;
            normalData[pixel * 4 + 3] = 255;
            ormData[pixel * 4 + 0] = 255;
            ormData[pixel * 4 + 1] = 128;
            ormData[pixel * 4 + 2] = 0;
            ormData[pixel * 4 + 3] = 255;
        }
        whiteTexture = CreateTextureFromData(device_.get(), WHITE_SIZE, WHITE_SIZE,
            reinterpret_cast<unsigned char*>(whiteData.data()), true);
        flatNormalTexture = CreateTextureFromData(
            device_.get(), WHITE_SIZE, WHITE_SIZE, normalData.data(), false);
        defaultORMTexture = CreateTextureFromData(
            device_.get(), WHITE_SIZE, WHITE_SIZE, ormData.data(), false);
        if (whiteTexture == handles::INVALID_RESOURCE ||
            flatNormalTexture == handles::INVALID_RESOURCE ||
            defaultORMTexture == handles::INVALID_RESOURCE) {
            std::cerr << "[TestModularPipeline] Failed to create fallback textures" << std::endl;
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
        ResourceHandle normalTexture = flatNormalTexture;
        if (!meshInfo.normalTexturePath.empty()) {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.normalTexturePath);
            if (!fullPath.empty()) {
                auto it = textureCache.find(fullPath);
                if (it != textureCache.end()) {
                    normalTexture = it->second;
                } else {
                    ResourceHandle loaded = LoadTextureFromFile(device_.get(), fullPath, false);
                    if (loaded != handles::INVALID_RESOURCE) {
                        normalTexture = loaded;
                        textureCache[fullPath] = loaded;
                    }
                }
            }
        }
        meshInfo.materialInstance->SetTexture(1, normalTexture);

        ResourceHandle ormTexture = defaultORMTexture;
        if (!meshInfo.ormTexturePath.empty()) {
            const std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.ormTexturePath);
            auto it = textureCache.find(fullPath);
            if (it != textureCache.end()) {
                ormTexture = it->second;
            } else {
                ResourceHandle loaded = LoadTextureFromFile(device_.get(), fullPath, false);
                if (loaded != handles::INVALID_RESOURCE) {
                    ormTexture = loaded;
                    textureCache[fullPath] = loaded;
                }
            }
        }
        meshInfo.materialInstance->SetTexture(2, ormTexture);
    }

    return true;
}

// T8.13 fix (card-registration loss): ApplyConfigChanges-driven Lumen
// rebuilds re-initialize SurfaceCachePass and wipe the card-generator's
// scene registrations (374 scene cards -> placeholder cards, SC goes and
// stays black). Registration is re-runnable, so the Run loop re-invokes
// this whenever the lookup count drops below the scene mesh count.
void TestModularPipeline::RegisterSceneCardsWithSurfaceCache() {
    if (!pipeline_ || sceneMeshes_.empty()) return;
    auto* scPass = pipeline_->GetSurfaceCachePass();
    if (!scPass || !scPass->IsInitialized()) return;
    auto& cardGen = scPass->GetCardGenerator();
    if (cardGen.GetLookupCount() >= sceneMeshes_.size()) return;  // intact

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
    std::cout << "[TestModularPipeline] SC cards re-registered: " << scRegistered
              << " meshes (lookups " << cardGen.GetLookupCount() << ")" << std::endl;
}

bool TestModularPipeline::LoadSponzaScene() {
    std::string baseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
    // Render the exact same scene payload as TestVulkanSponzaRenderGraph. The
    // much larger pipeline model remains the offline-SDF source only.
    std::string modelPath = baseDir + "Sponza.model";

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

    RenderLight sunLight;
    sunLight.type = LightType::Directional;
    sunLight.direction = {0.5f, -0.7f, 0.3f};
    sunLight.color = {1.0f, 0.95f, 0.9f};
    sunLight.intensity = 3.0f;
    scene_.AddLight(sunLight);

        RegisterSceneCardsWithSurfaceCache();

    return true;
}

// ============================================================================
// Run
// ============================================================================

void TestModularPipeline::Run() {
    if (!pipeline_ || isShutdown_) return;

    // T8.13 fix: re-register SC cards if a settings-driven Lumen rebuild
    // wiped the card-generator registrations (cheap count check per frame).
    if (pipeline_) {
        auto* scPass = pipeline_->GetSurfaceCachePass();
        if (scPass && scPass->IsInitialized() &&
            scPass->GetCardGenerator().GetLookupCount() < sceneMeshes_.size()) {
            RegisterSceneCardsWithSurfaceCache();
        }
    }

    // Update camera. T8.12b/T8.13 sync: SPGI_CAM_LOCK=1 freezes the camera —
    // the input source reads GLOBAL keyboard state, so a human using the
    // desktop leaks WASD into automated measurement runs (observed: strip
    // captures landing at drifted viewpoints). Match TestVulkanSponzaRenderGraph.
    if (!std::getenv("SPGI_CAM_LOCK")) {
        camera_.Update(0.016f);
    }
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
    view_.Cull(scene_);

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

#ifdef TEST_METAL_GI_FUSION
    // GI_VERIFY_FRAME 环境变量可调（默认 90）：探针收敛时间序列 A/B——
    // GI_VERIFY_FRAME=90 vs 3600 对比同探针 L0，判定探针更新回路是否生效
    // （"GI 区和第一秒没区别"的直接量化手段，2026-09-03）。
    {
        static u32 verifyFrame = [] {
            const char* e = std::getenv("GI_VERIFY_FRAME");
            return e ? (u32)std::atoi(e) : (u32)GI_VERIFY_FRAME;
        }();
        if (frameCount_ == verifyFrame) {
            RunGIFusionVerification(backBuffer);
        }
    }
#endif
}

#ifdef TEST_METAL_GI_FUSION
void TestModularPipeline::RunGIFusionVerification(ResourceHandle backBuffer) {
    using primal::graphics::rhi::BufferDesc;
    using primal::graphics::rhi::BufferType;
    using primal::graphics::rhi::CommandBufferHandle;
    using primal::graphics::rhi::CommandQueueType;
    using primal::graphics::rhi::ResourceBarrier;
    using primal::graphics::rhi::ResourceState;
    using primal::graphics::rhi::BufferTextureCopyRegion;
    using primal::graphics::rhi::handles::INVALID_RESOURCE;
    auto h2f = primal::graphics::pcg::PCGSDFReadbackManager::HalfToFloat;

    bool pass = true;
    auto lum = [](float r, float g, float b) {
        return 0.2126 * r + 0.7152 * g + 0.0722 * b;
    };

    // Generic RHI texture readback (Metal path — same RHI calls as Vulkan).
    auto readTextureStats = [&](const char* tag, ResourceHandle tex, u32 tw, u32 th,
                                bool halfFloat, u32 strideBytes,
                                double* outAvg, double* outMin, double* outMax) -> bool {
        if (tex == INVALID_RESOURCE) {
            std::cout << "[GIFusionVerify] " << tag << " INVALID" << std::endl;
            return false;
        }
        BufferDesc bd{};
        bd.size = (u64)tw * th * strideBytes;
        bd.type = BufferType::Raw;
        bd.memoryUsage = primal::graphics::rhi::GPUMemoryUsage::Readback;
        auto buf = device_->CreateBuffer(bd);
        if (buf == INVALID_RESOURCE) return false;
        auto cmdH = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        auto* cmd = primal::graphics::rhi::GetCommandBuffer(cmdH);
        if (!cmd || !cmd->Begin()) { device_->DestroyBuffer(buf); return false; }
        ResourceBarrier bk{};
        bk.resource = tex; bk.beforeState = ResourceState::ShaderResource;
        bk.afterState = ResourceState::CopySource;
        bk.subresource = 0xFFFFFFFF; bk.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&bk, 1);
        BufferTextureCopyRegion gn{};
        gn.imageSubresource = {0, 0, 1};
        gn.imageExtent = {tw, th, 1};
        cmd->CopyTextureToBuffer(tex, buf, &gn, 1);
        cmd->End();
        primal::graphics::rhi::QueueSubmitInfo si{};
        si.cmdBuffer = cmdH;
        device_->Submit(si);
        cmd->WaitForCompletion();
        device_->DestroyCommandBuffer(cmdH);
        auto* m = static_cast<u8*>(device_->MapBuffer(buf, 0, bd.size));
        if (!m) { device_->DestroyBuffer(buf); return false; }
        double sum = 0, mn = 1e30, mx = -1e30; u64 n = 0;
        for (u32 y = 0; y < th; y += 4) {
            for (u32 x = 0; x < tw; x += 4) {
                float r, g, b;
                if (halfFloat) {
                    const u16* px = reinterpret_cast<const u16*>(&m[((u64)y * tw + x) * strideBytes]);
                    r = h2f(px[0]); g = h2f(px[1]); b = h2f(px[2]);
                } else {
                    const u8* px = &m[((u64)y * tw + x) * strideBytes];
                    r = px[2] / 255.0f; g = px[1] / 255.0f; b = px[0] / 255.0f;
                }
                double l = lum(r, g, b);
                sum += l; if (l < mn) mn = l; if (l > mx) mx = l; ++n;
            }
        }
        device_->UnmapBuffer(buf);
        device_->DestroyBuffer(buf);
        *outAvg = sum / n; *outMin = mn; *outMax = mx;
        std::cout << "[GIFusionVerify] " << tag << " avg=" << *outAvg
                  << " min=" << *outMin << " max=" << *outMax << std::endl;
        return true;
    };

    // --- 1. Probe irradiance (host-visible buffer, direct map) ---------------
    double sh0Lum = -1.0;
    if (auto* ddgi = pipeline_->GetDDGIPass()) {
        auto irr = ddgi->GetIrradianceBuffer(0);
        if (irr != INVALID_RESOURCE) {
            // 读前 8 个探针的 L0：探针场是否真的被 trace/update 驱动。
            // 若全部等于静态种子（~1.05,1.05,1.23）→ update 未生效 →
            // GI 均匀灰、与直射光无关联（"GI 区是平的"的直测证据）。
            if (float* m = static_cast<float*>(device_->MapBuffer(irr, 0, 9 * 8 * sizeof(float)))) {
                // stride 修正：irradiance buffer 每探针存 9×float4（144B），
                // 旧读法 p*9 floats 读到的是错误位置的碎片。
                for (u32 p = 0; p < 8; ++p) {
                    const float* sh0 = m + p * 36;
                    std::cout << "[GIFusionVerify] probe[" << p << "] L0 rgb=("
                              << sh0[0] << "," << sh0[1] << "," << sh0[2] << ")" << std::endl;
                }
                sh0Lum = lum(m[0], m[1], m[2]);
                // sh0 RGB 分量（暖色温差排查）：Vulkan 基准探针场为暖色
                // (0.234,0.193,0.145) R/B≈1.6，Metal 中性 (0.215,0.212,0.209)
                // 意味着 SC/prev-screen 弹射未进入探针。
                std::cout << "[GIFusionVerify] probe sh0 rgb=(" << m[0] << "," << m[1]
                          << "," << m[2] << ")" << std::endl;

                // 相机附近探针 L0 时间序列（2026-09-03）：buffer 开头的 probe
                // 0..7 是网格角落——优先级按相机距离排序时它们会被永久饿死
                // （ageScore 封顶 0.3 竞争不过近距 ~1.0），读它们恒=种子。
                // 改读相机所在网格单元的 2×2×2 邻域，才能反映"可见 GI 区"
                // 的探针是否真的在更新。
                if (auto* ddgi2 = pipeline_->GetDDGIPass()) {
                    const auto vol = ddgi2->GetVolumeData();
                    if (vol.ProbeCountTotal > 0 && vol.ProbeSpacing > 0.0f &&
                        vol.ProbeCounts[0] > 0 && vol.ProbeCounts[1] > 0 && vol.ProbeCounts[2] > 0) {
                        const math::v3 campos = camera_.GetPosition();
                        auto gcoord = [&](float w, u32 axis) -> int {
                            float rel = (axis == 0 ? w - vol.ProbeOrigin.x
                                       : axis == 1 ? w - vol.ProbeOrigin.y
                                                   : w - vol.ProbeOrigin.z) / vol.ProbeSpacing;
                            int c = (int)std::lround(rel);
                            u32 n = vol.ProbeCounts[axis];
                            return std::min(std::max(c, 0), (int)n - 1);
                        };
                        int cx = gcoord(campos.x, 0);
                        int cy = gcoord(campos.y, 1);
                        int cz = gcoord(campos.z, 2);
                        auto idx = [&](int x, int y, int z) -> u32 {
                            return (u32)(x + vol.ProbeCounts[0] * (y + vol.ProbeCounts[1] * z));
                        };
                        std::cout << "[GIFusionVerify] vol: ProbeOrigin=("
                                  << vol.ProbeOrigin.x << "," << vol.ProbeOrigin.y << ","
                                  << vol.ProbeOrigin.z << ") SdfCascadeCount="
                                  << vol.SdfCascadeCount
                                  << " SdfOrigin0=(" << vol.SdfOrigins[0].x << ","
                                  << vol.SdfOrigins[0].y << "," << vol.SdfOrigins[0].z << ")"
                                  << " SdfExtent0=(" << vol.SdfExtents[0].x << ","
                                  << vol.SdfExtents[0].y << "," << vol.SdfExtents[0].z << ")"
                                  << " SdfVoxel0=" << vol.SdfVoxelSizes[0].x
                                  << " RayMaxDist=" << vol.RayMaxDistance
                                  << " RaySampleMode=" << vol.RaySampleMode
                                  << std::endl;
                        std::cout << "[GIFusionVerify] camera-cell probes (gc="
                                  << cx << "," << cy << "," << cz << "):" << std::endl;
                        for (int dz = 0; dz <= 1; ++dz)
                            for (int dy = 0; dy <= 1; ++dy)
                                for (int dx = 0; dx <= 1; ++dx) {
                                    u32 pi = idx(cx + dx, cy + dy, cz + dz);
                                    if (pi >= vol.ProbeCountTotal) continue;
                                    const float* sh = m + pi * 36;
                                    std::cout << "  probe[" << pi << "] L0 rgb=("
                                              << sh[0] << "," << sh[1] << "," << sh[2] << ")"
                                              << " L1=(" << sh[4] << "," << sh[5] << "," << sh[6] << ")"
                                              << std::endl;
                                }

                        // DDGI_PATH_DEBUG 构建下：解码 ray_buffer 的路径标签
                        // （w = 40 + path*0.001，path: 0=miss 1=SC 2=prev 3=analytic），
                        // 聚合相机邻域 8 探针 × 全部射线的路径直方图。
                        if (std::getenv("DDGI_PATH_DEBUG")) {
                            if (auto rayBuf = ddgi2->GetRayDataBuffer();
                                rayBuf != rhi::handles::INVALID_RESOURCE &&
                                vol.RaysPerProbe > 0) {
                                const u64 raysBytes = u64(vol.ProbeCountTotal) * vol.RaysPerProbe * 16u;
                                if (float* rb = static_cast<float*>(
                                        device_->MapBuffer(rayBuf, 0, raysBytes))) {
                                    u32 hist[4] = {0, 0, 0, 0};
                                    for (int dz = 0; dz <= 1; ++dz)
                                        for (int dy = 0; dy <= 1; ++dy)
                                            for (int dx = 0; dx <= 1; ++dx) {
                                                u32 pi = idx(cx + dx, cy + dy, cz + dz);
                                                if (pi >= vol.ProbeCountTotal) continue;
                                                for (u32 r = 0; r < vol.RaysPerProbe; ++r) {
                                                    float w = rb[u64(pi) * vol.RaysPerProbe + r * 4u + 3u];
                                                    if (w < 0.0f) { hist[0]++; continue; }
                                                    u32 path = (u32)std::lround((w - 40.0f) * 1000.0f);
                                                    if (path < 4u) hist[path]++;
                                                }
                                            }
                                    device_->UnmapBuffer(rayBuf);
                                    std::cout << "[GIFusionVerify] ray-path hist:"
                                              << " miss=" << hist[0]
                                              << " SC=" << hist[1]
                                              << " prev=" << hist[2]
                                              << " analytic=" << hist[3] << std::endl;
                                }
                            }
                        }

                        // SC grid header 直读：验证 grid 覆盖/卡片数
                        if (auto sgo = ddgi2->GetSCGridOffsetsBuffer();
                            sgo != rhi::handles::INVALID_RESOURCE) {
                            if (u32* gh = static_cast<u32*>(
                                    device_->MapBuffer(sgo, 0, 16 * sizeof(u32)))) {
                                auto u2f = [](u32 v) { float f; memcpy(&f, &v, 4); return f; };
                                std::cout << "[GIFusionVerify] SC grid: min=("
                                          << u2f(gh[0]) << "," << u2f(gh[1]) << "," << u2f(gh[2]) << ")"
                                          << " cards=" << gh[3]
                                          << " max=(" << u2f(gh[4]) << "," << u2f(gh[5]) << "," << u2f(gh[6]) << ")"
                                          << " cellSize=" << u2f(gh[7])
                                          << " dims=(" << gh[8] << "," << gh[9] << "," << gh[10] << ")"
                                          << std::endl;
                                // 检查相机位置处的 cell
                                float cs = u2f(gh[7]);
                                float gminx = u2f(gh[0]), gminy = u2f(gh[1]), gminz = u2f(gh[2]);
                                u32 gdx = gh[8], gdy = gh[9], gdz = gh[10];
                                auto campos = camera_.GetPosition();
                                int cx2 = int((campos.x - gminx) / cs);
                                int cy2 = int((campos.y - gminy) / cs);
                                int cz2 = int((campos.z - gminz) / cs);
                                bool inside = cx2 >= 0 && cy2 >= 0 && cz2 >= 0 &&
                                              cx2 < int(gdx) && cy2 < int(gdy) && cz2 < int(gdz);
                                std::cout << "[GIFusionVerify] SC camera cell: pos=("
                                          << campos.x << "," << campos.y << "," << campos.z << ")"
                                          << " cell=(" << cx2 << "," << cy2 << "," << cz2 << ")"
                                          << " inside=" << (inside ? "YES" : "NO")
                                          << std::endl;
                                if (inside) {
                                    u32 ci = cx2 + gdx * (cy2 + gdy * cz2);
                                    u32 begin = gh[16 + ci];
                                    u32 end = gh[17 + ci];
                                    std::cout << "[GIFusionVerify] SC cell[" << ci << "] cards="
                                              << (end - begin) << " (range [" << begin << "," << end << "))"
                                              << std::endl;
                                }
                                device_->UnmapBuffer(sgo);
                            }
                        }

                        // update list 直读：相机邻域 8 探针是否在列表内
                        if (auto ulBuf = ddgi2->GetProbeUpdateListBuffer(frameCount_ % 3);
                            ulBuf != rhi::handles::INVALID_RESOURCE) {
                            u32 listCap = vol.ProbeCountTotal;
                            if (u32* ul = static_cast<u32*>(
                                    device_->MapBuffer(ulBuf, 0, u64(listCap) * 4u))) {
                                u32 mn = 0xFFFFFFFFu, mx = 0;
                                u32 inCell = 0;
                                u32 camSlot[8]; u32 camSlotN = 0;
                                for (u32 k = 0; k < vol.ProbeUpdateCount && k < listCap; ++k) {
                                    mn = std::min(mn, ul[k]);
                                    mx = std::max(mx, ul[k]);
                                    for (int dz = 0; dz <= 1; ++dz)
                                        for (int dy = 0; dy <= 1; ++dy)
                                            for (int dx = 0; dx <= 1; ++dx)
                                                if (ul[k] == idx(cx + dx, cy + dy, cz + dz)) {
                                                    inCell++;
                                                    if (camSlotN < 8) camSlot[camSlotN++] = k;
                                                }
                                }
                                device_->UnmapBuffer(ulBuf);
                                std::cout << "[GIFusionVerify] update-list: n="
                                          << vol.ProbeUpdateCount << " idxRange=["
                                          << mn << "," << mx << "] cameraCellInList="
                                          << inCell << "/8" << std::endl;

                                // hit_distance 直读（按更新槽位！trace 以槽位写入，
                                // finalize 消费同槽位——这是相机探针的真实 march 数据）
                                if (auto hdf2 = ddgi2->GetHitDistanceBuffer();
                                    hdf2 != rhi::handles::INVALID_RESOURCE && vol.RaysPerProbe >= 2) {
                                    const u64 hdBytes = u64(vol.ProbeCountTotal) * vol.RaysPerProbe * 4u;
                                    if (float* hd = static_cast<float*>(
                                            device_->MapBuffer(hdf2, 0, hdBytes))) {
                                        // 同帧配对：hd（trace 写，槽位索引）vs
                                        // ray_buffer（finalize 写，绝对探针索引）
                                        float* rb2 = nullptr;
                                        rhi::ResourceHandle rbBuf2 = ddgi2->GetRayDataBuffer();
                                        if (rbBuf2 != rhi::handles::INVALID_RESOURCE)
                                            rb2 = static_cast<float*>(
                                                device_->MapBuffer(rbBuf2, 0, hdBytes * 4u));
                                        for (u32 s = 0; s < camSlotN; ++s) {
                                            u64 o = u64(camSlot[s]) * vol.RaysPerProbe;
                                            u32 probeIdx = ul[camSlot[s]];
                                            u64 po = u64(probeIdx) * vol.RaysPerProbe * 4u;
                                            u32 missN = 0, hitN = 0;
                                            for (u32 r = 0; r < vol.RaysPerProbe; ++r) {
                                                if (hd[o + r] < 0.0f) missN++; else hitN++;
                                            }
                                            std::cout << "[GIFusionVerify] camSlot[" << camSlot[s]
                                                      << "] probe=" << probeIdx
                                                      << " hd.ray0=" << hd[o]
                                                      << " hd.ray1=" << hd[o + 1]
                                                      << " hd.hits=" << hitN << "/128"
                                                      << " hd.miss=" << missN
                                                      << " | rb.w[0..3]="
                                                      << rb2[po + 3] << ","
                                                      << rb2[po + 7] << ","
                                                      << rb2[po + 11] << ","
                                                      << rb2[po + 15]
                                                      << " rb.missN="
                                                      << [&]{u32 n=0; for(u32 r=0;r<vol.RaysPerProbe;++r) if (rb2[po + r*4u+3u] < 0.0f) ++n; return n;}()
                                                      << "/128" << std::endl;
                                        }
                                        if (rb2) device_->UnmapBuffer(rbBuf2);
                                        device_->UnmapBuffer(hdf2);
                                    }
                                }
                            }
                        }

                        // hit_distance 直读：ray0=探针原点原始 SDF 距离
                        // （5555=级联外哨兵）、ray1=重定位后距离、ray2+=march 结果。
                        if (auto hdf = ddgi2->GetHitDistanceBuffer();
                            hdf != rhi::handles::INVALID_RESOURCE && vol.RaysPerProbe >= 3) {
                            const u64 hdBytes = u64(vol.ProbeCountTotal) * vol.RaysPerProbe * 4u;
                            if (float* hd = static_cast<float*>(
                                    device_->MapBuffer(hdf, 0, hdBytes))) {
                                for (int dz = 0; dz <= 1; ++dz)
                                    for (int dy = 0; dy <= 1; ++dy)
                                        for (int dx = 0; dx <= 1; ++dx) {
                                            u32 pi = idx(cx + dx, cy + dy, cz + dz);
                                            if (pi >= vol.ProbeCountTotal) continue;
                                            u64 o = u64(pi) * vol.RaysPerProbe;
                                            std::cout << "[GIFusionVerify] hd[" << pi
                                                      << "] origin=" << hd[o]
                                                      << " reloc=" << hd[o + 1]
                                                      << " ray2=" << hd[o + 2]
                                                      << " ray3=" << hd[o + 3] << std::endl;
                                        }
                                device_->UnmapBuffer(hdf);
                            }
                        }
                    }
                }
                device_->UnmapBuffer(irr);
            }
        }
    }
    std::cout << "[GIFusionVerify] probe sh0 lum=" << sh0Lum << " (Vulkan ref 0.355)" << std::endl;

    // --- 2. Gather output variation ------------------------------------------
    double gAvg = 0, gMin = 0, gMax = 0;
    bool gatherOk = false;
    if (auto* gm = pipeline_->GetGIGatherModule())
        // Metal gather 已升全分辨率——按模块实际输出尺寸读回。
        gatherOk = readTextureStats("gather(GI,half)", gm->GetOutputTexture(),
                                    gm->GetGatherWidth(), gm->GetGatherHeight(), true, 8,
                                    &gAvg, &gMin, &gMax);

    // --- 3. Final composite ----------------------------------------------------
    double fAvg = 0, fMin = 0, fMax = 0;
    bool finalOk = readTextureStats("final(backbuffer)", backBuffer,
                                    renderWidth_, renderHeight_, false, 4,
                                    &fAvg, &fMin, &fMax);

    // --- Alignment assertions (bands from the fixed Vulkan reference) --------
    // Recalibrated for the SC-first hierarchy (surface cache → world probes →
    // screen probes): probes now carry SC-projected energy (previously the
    // 20B/32B struct mismatch left the Metal SC feed near-black), the gather
    // is deliberately smoother (old ≥3 variation band encoded the spiky
    // single-probe-fallback look), and the hierarchical fusion lifts the
    // composite brightness versus the old flat blend.
    auto checkBand = [&](const char* what, double v, double lo, double hi) {
        bool ok = (v >= lo && v <= hi);
        std::cout << "[GIFusionVerify] " << (ok ? "PASS" : "FAIL") << " " << what
                  << "=" << v << " ∈ [" << lo << ", " << hi << "]" << std::endl;
        if (!ok) pass = false;
    };
    checkBand("probe_sh0_lum", sh0Lum, 0.08, 1.5);
    if (gatherOk) {
        double variation = (gAvg > 1e-6) ? (gMax / gAvg) : 0.0;
        checkBand("gather_variation(max/avg)", variation, 1.2, 1e9);
        checkBand("gather_avg", gAvg, 0.005, 0.8);
    } else { pass = false; }
    if (finalOk) {
        checkBand("final_dynamic_range(max-min)", fMax - fMin, 0.1, 1e9);
        checkBand("final_avg", fAvg, 0.005, 0.65);
    } else { pass = false; }

    std::cout << "[GIFusionVerify] " << (pass ? "ALL ALIGNED ✔" : "MISALIGNED ✘")
              << " — Metal GI composite vs Vulkan reference" << std::endl;
    std::cout.flush(); std::cerr.flush();
    // _Exit 而非 std::exit：验证在帧回调中途结束进程，std::exit 会跑静态
    // 析构——window_info free_list 析构 assert（窗口未走正常 Shutdown 路径）
    // 必然触发 abort(134)。输出已 flush，_Exit 跳过析构直接退出。
    _Exit(pass ? 0 : 1);
}
#endif

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

    if (iblPrecomputer_) {
        iblPrecomputer_->Shutdown();
        iblPrecomputer_.reset();
    }
    auto destroyIBLTexture = [&](ResourceHandle& texture) {
        if (texture != handles::INVALID_RESOURCE) {
            device_->DestroyTexture(texture);
            texture = handles::INVALID_RESOURCE;
        }
    };
    destroyIBLTexture(iblIrradiance_);
    destroyIBLTexture(iblPrefilter_);
    destroyIBLTexture(iblBrdfLUT_);
    destroyIBLTexture(envCube_);

    if (offline_sdf_merger_) {
        delete offline_sdf_merger_;
        offline_sdf_merger_ = nullptr;
    }
    renderSystem_.Shutdown();

    if (window_.is_valid()) {
        primal::platform::remove_window(window_.get_id());
    }

    device_.reset();
    primal::content::shutdown();
    primal::content::AsyncResourceLoader::Shutdown();
    primal::jobsystem::JobSystem::Shutdown();
}
