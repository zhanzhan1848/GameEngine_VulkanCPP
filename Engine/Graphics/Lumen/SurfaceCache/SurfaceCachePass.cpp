#include "SurfaceCachePass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <set>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::lumen {

using namespace rhi;
using namespace rendergraph;
namespace rhimath = primal::graphics::rhi::math;

namespace {

// ============================================================================
// Descriptor update helper (same pattern as LumenDDGIPass)
// ============================================================================

struct DescriptorData {
    uint32_t binding;
    DescriptorType type;
    ResourceHandle resource;
    uint32_t count = 1;
};

static void UpdateDescriptorSet(RHIDeviceBase* device, DescriptorSetHandle set,
                                const DescriptorData* params, uint32_t count) {
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
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else if (params[i].type == DescriptorType::SampledImage ||
                   params[i].type == DescriptorType::SampledDepthImage ||
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

// ============================================================================
// Shader source loader with #include resolution
// (Same pattern as LumenDDGIPass — Metal's newLibrary(source) can't resolve
// #include without include dirs, so we manually inline local includes.)
// ============================================================================

static const std::string SHADER_BASE_DIR =
    utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Metal/shaders/") + "/";
static const std::string SC_SHADER_DIR = SHADER_BASE_DIR + "Lumen/";

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Recursively resolve #include "..." directives by inlining file content.
// Only resolves local includes (quoted), not system includes (angle brackets).
static std::string ResolveIncludes(const std::string& source, const std::string& baseDir,
                                   std::set<std::string>& included) {
    std::istringstream in(source);
    std::ostringstream out;
    std::string line;

    while (std::getline(in, line)) {
        std::string trimmed = line;
        size_t firstNonSpace = trimmed.find_first_not_of(" \t");
        if (firstNonSpace != std::string::npos) trimmed = trimmed.substr(firstNonSpace);

        if (trimmed.find("#include \"") == 0) {
            size_t start = trimmed.find('"') + 1;
            size_t end = trimmed.find('"', start);
            if (start != std::string::npos && end != std::string::npos) {
                std::string includeFile = trimmed.substr(start, end - start);

                std::string fullPath = baseDir + includeFile;
                if (std::ifstream(fullPath).good() == false) {
                    fullPath = SHADER_BASE_DIR + includeFile;
                }

                if (included.find(fullPath) == included.end()) {
                    included.insert(fullPath);
                    std::string includedContent = ReadFileToString(fullPath);
                    if (!includedContent.empty()) {
                        std::string resolved = ResolveIncludes(includedContent,
                            fullPath.substr(0, fullPath.find_last_of('/') + 1), included);
                        out << resolved << "\n";
                    } else {
                        PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] Warning: Failed to read include: " << fullPath << std::endl;
                        out << line << "\n";
                    }
                }
                continue;
            }
        }
        out << line << "\n";
    }
    return out.str();
}

static std::vector<u8> LoadShaderBytecode(rhi::RHIDeviceBase* device, const char* shaderName) {
    const auto platform = device ? device->GetPlatform() : rhi::RHIPlatform::Metal;

    if (platform == rhi::RHIPlatform::Vulkan) {
        // Load precompiled SPIR-V (.comp.spv) from Vulkan/shaders/Lumen/
        const std::string relPath =
            utils::ShaderRegistry::GetShaderBaseDir(platform) + "Lumen/" + shaderName + ".comp.spv";
        const std::vector<std::string> candidates = {
            relPath,
            utils::ShaderRegistry::ResolveShaderPath(relPath),
        };
        for (const auto& path : candidates) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) continue;
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<u8> bytecode(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytecode.data()), size)) continue;
            return bytecode;
        }
        PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] Failed to load SPIR-V: " << shaderName << std::endl;
        return {};
    }

    // Metal path: load .metal source + resolve #includes
    std::string shaderPath = SC_SHADER_DIR + shaderName + ".metal";
    std::string source = ReadFileToString(shaderPath);
    if (source.empty()) {
        shaderPath = std::string("Engine/Graphics/Metal/shaders/Lumen/") + shaderName + ".metal";
        source = ReadFileToString(shaderPath);
    }
    if (source.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] Failed to load shader: " << shaderName << std::endl;
        return {};
    }
    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, SC_SHADER_DIR, included);
    return std::vector<u8>(resolved.begin(), resolved.end());
}

// ============================================================================
// Per-pass data struct for the RenderGraph pass
// ============================================================================

struct SurfaceCacheRGData {
    RGResourceHandle prev_frame_color;
};

} // anonymous namespace

// ============================================================================
// SurfaceCachePass Implementation
// ============================================================================

float SurfaceCachePass::DebugShadowVisibilityAt(u32 index) const {
    if (shadow_visibility_buffer_ == handles::INVALID_RESOURCE || !device_) return -1.0f;
    auto* p = static_cast<const float*>(device_->MapBuffer(shadow_visibility_buffer_));
    if (!p) return -2.0f;
    float v = p[index];
    device_->UnmapBuffer(shadow_visibility_buffer_);
    return v;
}

SurfaceCachePass::~SurfaceCachePass() {
    Shutdown();
}

// X2/A3: 时域策略枚举存储。SC 自身无 probe 时域链（LightEval 页预算走
// round-robin 游标），当前所有枚举值都不改变行为；X3 归并入五轴 GI 配置。
void SurfaceCachePass::SetTemporalStrategy(GITemporalStrategy strategy) {
    if (temporal_strategy_ == strategy) return;
    temporal_strategy_ = strategy;
    PRIMAL_LOG_STREAM(Info, "Render") << "[SurfaceCache] Temporal strategy -> "
              << GITemporalStrategyName(strategy) << " (stored; takes effect in X3)" << std::endl;
}

bool SurfaceCachePass::Initialize(RHIDeviceBase* device, const LumenConfig& config) {
    if (initialized_) return true;

    device_ = device;
    config_ = config;
    atlas_size_ = config.surface_cache_atlas_size;
    page_size_ = config.surface_cache_page_size;
    light_eval_rr_cursor_ = 0;
    lighting_atlas_seed_mask_ = 0;
    atlas_seeded_ = false;

    // Initialize card generator (meshes registered later by the pipeline
    // when the scene snapshot rebuilds — real per-mesh AABBs, not test data).
    card_generator_.Initialize(device, atlas_size_, page_size_, config.surface_cache_max_cards);

    // Create atlas textures
    CreateAtlasTextures();

    // T8.12: card-view capture — DEFAULT ON (validated 2026-08-28: L_2F
    // 39.4→46.6 median, REDBOX 9.4→16.6, colored bounce visible, 0 reg).
    // SC_CARDVIEW=0 falls back to the legacy screen-space gather.
    const char* cvEnv = std::getenv("SC_CARDVIEW");
    card_view_enabled_ = !(cvEnv && strcmp(cvEnv, "0") == 0);
    if (card_view_enabled_) {
        rhi::TextureDesc dv{};
        dv.size = { atlas_size_, atlas_size_, 1 };
        dv.mipLevels = 1;
        dv.arraySize = 1;
        dv.format = DataFormat::D32_Float;
        dv.type = TextureType::Texture2D;
        dv.usage = TextureUsage::DepthStencil;
        dv.name = "SurfaceCache_CardViewDepth";
        card_view_depth_ = device_->CreateTexture(dv);
        PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] T8.12 card-view capture ENABLED (depth "
                  << (card_view_depth_ != rhi::handles::INVALID_RESOURCE ? "ok" : "FAILED")
                  << ")" << std::endl;
    }

    // Create triple-buffered constant buffers
    CreateConstantBuffers();

    // Create descriptor set layouts
    CreateDescriptorSetLayouts();

    // Create pipelines and triple-buffered descriptor sets
    CreatePipelines();

    initialized_ = true;

    PRIMAL_LOG_STREAM(Info, "Render") << "[SurfaceCache] Initialized (atlas: "
              << atlas_size_ << "x" << atlas_size_
              << ", page: " << page_size_
              << ", max_cards: " << config.surface_cache_max_cards << ")" << std::endl;
    return true;
}

void SurfaceCachePass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;

    card_view_depth_ = rhi::handles::INVALID_RESOURCE;

    device_ = nullptr;

    // No explicit GPU resource destruction needed -- handles are POD types
    // managed by the RHI device's garbage collector.
}

// ============================================================================
// Private helper methods
// ============================================================================

void SurfaceCachePass::CreateAtlasTextures() {
    auto CreateAtlas = [&](const char* name, DataFormat format, ResourceHandle& handle, bool renderTarget = false) {
        TextureDesc desc{};
        desc.size = { atlas_size_, atlas_size_, 1 };
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = format;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess |
                     TextureUsage::CopySource;
        if (renderTarget) desc.usage = desc.usage | TextureUsage::RenderTarget;
        desc.name = name;
        handle = device_->CreateTexture(desc);
    };

    // Albedo: RGBA8 (albedo + alpha) — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Albedo", DataFormat::RGBA8_UNorm, albedo_atlas_, true);

    // Normal: RG16F (octahedral-encoded normal) — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Normal", DataFormat::RG16_Float, normal_atlas_, true);

    // Depth: R32F — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Depth", DataFormat::R32_Float, depth_atlas_, true);

    // Emissive: RGBA16F — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Emissive", DataFormat::RGBA16_Float, emissive_atlas_, true);

    // Lighting atlas: triple-buffered (RGBA16F for HDR lighting)
    for (int i = 0; i < 3; ++i) {
        CreateAtlas(
            ("SurfaceCache_Lighting_" + std::to_string(i)).c_str(),
            DataFormat::RGBA16_Float,
            lighting_atlas_[i]);
    }

    // Runtime buffers
    {
        // Light info buffer: light parameters for evaluation
        BufferDesc lightInfoDesc{};
        lightInfoDesc.size = 256 * sizeof(u32) * 4;  // Max 256 lights, 64 bytes each
        lightInfoDesc.type = BufferType::Structured;
        lightInfoDesc.usage = GPUMemoryUsage::Dynamic;
        lightInfoDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        light_info_buffer_ = device_->CreateBuffer(lightInfoDesc);
    }

    {
        // Ray hits buffer: for indirect tracing
        u32 pagesPerSide = atlas_size_ / page_size_;
        u32 totalPages = pagesPerSide * pagesPerSide;
        BufferDesc rayHitsDesc{};
        rayHitsDesc.size = (u64)totalPages * page_size_ * page_size_ * sizeof(float) * 4; // RGBA per texel
        rayHitsDesc.type = BufferType::Structured;
        rayHitsDesc.usage = GPUMemoryUsage::Dynamic;
        rayHitsDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        ray_hits_buffer_ = device_->CreateBuffer(rayHitsDesc);
    }

    {
        // Card dispatch buffer: flattened per-card texel dispatch info
        BufferDesc desc{};
        desc.size = (u64)config_.surface_cache_max_cards * sizeof(CardDispatchInfo);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        card_dispatch_buffer_ = device_->CreateBuffer(desc);
    }
}

void SurfaceCachePass::CreateConstantBuffers() {
    auto CreateCBs = [&](ResourceHandle (&cbs)[3], u64 size) {
        for (int i = 0; i < 3; i++) {
            BufferDesc desc{};
            desc.size = size;
            desc.type = BufferType::Constant;
            desc.usage = GPUMemoryUsage::Dynamic;
            desc.memoryUsage = GPUMemoryUsage::Dynamic;
            cbs[i] = device_->CreateBuffer(desc);
        }
    };

    CreateCBs(global_cb_, 512);    // GlobalShaderData (padded)
    CreateCBs(params_cb_, 512);    // SurfaceCacheParams (padded)
    CreateCBs(capture_cb_, 96);    // CaptureParams: invViewProj (64) + counts (16) padded
    CreateCBs(card_fill_cb_, 176); // CardFillParams: SDF cascades + light + counts
    CreateCBs(shadow_cb_, 176);    // T3.3: ShadowParams (SDF cascades + light + counts)

    // T3.3: atlas-space visibility buffer (float per atlas texel), CPU-
    // initialized to 1.0 so untraced texels / SDF-unavailable frames stay lit.
    if (shadow_visibility_buffer_ == handles::INVALID_RESOURCE && atlas_size_ > 0) {
        BufferDesc visDesc{};
        visDesc.size = (u64)atlas_size_ * atlas_size_ * sizeof(float);
        visDesc.type = BufferType::Structured;
        visDesc.usage = GPUMemoryUsage::Dynamic;
        visDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        visDesc.structured.elementCount = atlas_size_ * atlas_size_;
        visDesc.structured.elementStride = sizeof(float);
        shadow_visibility_buffer_ = device_->CreateBuffer(visDesc);
        auto* vis = static_cast<float*>(device_->MapBuffer(shadow_visibility_buffer_));
        if (vis) {
            for (u64 i = 0; i < (u64)atlas_size_ * atlas_size_; ++i) vis[i] = 1.0f;
            device_->UnmapBuffer(shadow_visibility_buffer_);
        }
    }
}

void SurfaceCachePass::CreateDescriptorSetLayouts() {
    const bool isVk = device_->GetPlatform() == rhi::RHIPlatform::Vulkan;

    // --- Dilate: 2 textures (depth_in read + depth_out write) + 1 UBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // depth_in
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // depth_out
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params
        };
        dilate_set_layout_ = device_->CreateDescriptorSetLayout({3, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_in  [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_out [[texture(1)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params    [[buffer(1)]]
        };
        DescriptorSetLayoutDesc layoutDesc{3, bindings};
        dilate_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- LightCull: 2 textures (depth + normal atlas) + 1 UBO + 2 SSBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // depth_atlas
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // LightCullParams
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lights
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // tile_assignment
        };
        light_cull_set_layout_ = device_->CreateDescriptorSetLayout({5, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_atlas  [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // normal_atlas [[texture(1)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params       [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lights       [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // tile_assignment [[buffer(3)]]
        };
        DescriptorSetLayoutDesc layoutDesc{5, bindings};
        light_cull_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- LightEval (merged): 4 textures + 1 UBO + 3 SSBO + 1 prev_lighting + 1 shadow_vis + 1 ddgi_irradiance + 1 depth_atlas ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // albedo_atlas
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // emissive_atlas
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // lighting_out
            {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lights
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_dispatch
            {8, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // prev_lighting (multi-bounce)
            {9, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // shadow_visibility (T3.3)
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // ddgi irradiance (probe-fed indirect)
            {11, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // depth_atlas (surface-point probe gather)
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[9].readonly = true;
        b[10].readonly = true;
        light_eval_set_layout_ = device_->CreateDescriptorSetLayout({12, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // albedo_atlas  [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // normal_atlas  [[texture(1)]]
            {2, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // emissive_atlas [[texture(2)]]
            {3, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // lighting_out  [[texture(3)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params        [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards         [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lights        [[buffer(3)]]
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_dispatch [[buffer(4)]]
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // shadow_visibility [[buffer(5)]] (T3.3)
            // T8.13 Vulkan↔Metal sync: probe-fed indirect + multi-bounce feedback
            // + surface-point depth — same resources as the Vulkan layout.
            {4, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // prev_lighting [[texture(4)]] (multi-bounce)
            {5, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_atlas   [[texture(5)]]
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ddgi irradiance [[buffer(6)]] (probe-fed indirect)
        };
        DescriptorSetLayoutBinding* bm = bindings;
        bm[5].readonly = true;
        bm[8].readonly = true;  // ddgi irradiance
        DescriptorSetLayoutDesc layoutDesc{12, bindings};
        light_eval_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Capture (graphics): 3 textures (material arrays) + 1 UBO + 1 SSBO ---
    // Metal-only until Capture.vert/frag SPIR-V ports exist.
    if (!isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},     // albedo_array  [[texture(0)]]
            {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},     // normal_array  [[texture(1)]]
            {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},     // orm_array     [[texture(2)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel, nullptr}, // pass_data [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex, nullptr},   // instances     [[buffer(2)]]
            // M4.4:VT 绑定(可选 — 系统未启用时绑零值纹理,flags 无 VT 位
            // → shader 走 array 回退)
            {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},     // vt_page_table [[texture(3)]]
            {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},     // vt_atlas      [[texture(4)]]
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Pixel, nullptr},    // vt_params     [[buffer(5)]]
        };
        DescriptorSetLayoutDesc layoutDesc{8, bindings};
        capture_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- IndirectTrace: 5 textures (depth+normal+3 SDF) + 1 UBO + 1 UBO + 2 SSBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // depth_atlas
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf0 (3D)
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf1 (3D)
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf2 (3D)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global_cb
            {6, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_hits
        };
        // Mark SDF textures as 3D view dimension
        bindings[2].is3D = true;
        bindings[3].is3D = true;
        bindings[4].is3D = true;
        indirect_trace_set_layout_ = device_->CreateDescriptorSetLayout({9, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_atlas  [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // normal_atlas [[texture(1)]]
            {2, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf0         [[texture(2)]]
            {3, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf1         [[texture(3)]]
            {4, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf2         [[texture(4)]]
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global_cb    [[buffer(0)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params       [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards        [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_hits     [[buffer(3)]]
        };
        DescriptorSetLayoutDesc layoutDesc{9, bindings};
        indirect_trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- IndirectResolve: 4 textures + 1 UBO + 1 UBO + 3 SSBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev_lighting
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // albedo_atlas
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sky_cubemap (dummy)
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // indirect_out
            {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global_cb
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lookups
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_hits
        };
        indirect_resolve_set_layout_ = device_->CreateDescriptorSetLayout({9, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // prev_lighting [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // albedo_atlas  [[texture(1)]]
            {2, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sky_cubemap   [[texture(2)]]
            {3, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // indirect_out  [[texture(3)]]
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global_cb     [[buffer(0)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params        [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // lookups       [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards         [[buffer(3)]]
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_hits      [[buffer(4)]]
        };
        DescriptorSetLayoutDesc layoutDesc{9, bindings};
        indirect_resolve_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- AtlasInit: 4 write images + 1 UBO + 1 SSBO ---
    // Populates card atlas regions with default material data. T8.13 sync:
    // enabled on Metal too — heap-allocated Metal atlases start as garbage and
    // the SC↔probe feedback loop latches it (SC collapsed to black in ~90
    // frames without this seed).
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // albedo_atlas
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // depth_atlas
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // emissive_atlas
            {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // FlattenedLightingParams
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_dispatch
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[5].readonly = true;
        atlas_init_set_layout_ = device_->CreateDescriptorSetLayout({6, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // albedo_atlas   [[texture(0)]]
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas   [[texture(1)]]
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // depth_atlas    [[texture(2)]]
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // emissive_atlas [[texture(3)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params         [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_dispatch  [[buffer(2)]]
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[5].readonly = true;
        atlas_init_set_layout_ = device_->CreateDescriptorSetLayout({6, bindings});
    }

    // --- Capture (GBuffer-gather, Vulkan-only): 3 GBuffer inputs + 3 atlas
    //     write images + 1 UBO + 1 cards SSBO + 2 card-grid SSBOs (T1.3) ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},  // gbuffer_depth
            {1, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // gbuffer_albedo
            {2, DescriptorType::SampledImage,      1, ShaderStage::Compute, nullptr},  // gbuffer_normal
            {3, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // albedo_atlas
            {4, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // normal_atlas
            {5, DescriptorType::StorageImage,      1, ShaderStage::Compute, nullptr},  // depth_atlas
            {6, DescriptorType::UniformBuffer,     1, ShaderStage::Compute, nullptr},  // CaptureParams
            {7, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // cards
            {8, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // sc_grid_offsets
            {9, DescriptorType::StorageBuffer,     1, ShaderStage::Compute, nullptr},  // sc_grid_cards
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[7].readonly = true;
        b[8].readonly = true;
        b[9].readonly = true;
        capture_gather_set_layout_ = device_->CreateDescriptorSetLayout({10, bindings});
    }

    // --- CardFill (SDF-based, Vulkan-only): 3 SDF 3D + 4 atlas + 1 UBO + 4 SSBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf_cascade_2
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // albedo_atlas
            {4, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas
            {5, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // depth_atlas
            {6, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // lighting_out
            {7, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // CardFillParams
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // card_dispatch
            {9, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // instance_data
            {11, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // material_data
            {12, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr}, // albedo texture array
            {13, DescriptorType::Sampler,       1, ShaderStage::Compute, nullptr}, // sampler
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[0].is3D = true; b[1].is3D = true; b[2].is3D = true;
        b[8].readonly = true;
        b[9].readonly = true;
        b[10].readonly = true;
        b[11].readonly = true;
        b[12].isArray = true;  // texture2DArray
        // M4.4:VT 绑定(14/15/16 — 可选,系统未启用时绑零值;
        // MaterialData.flags 的 VT 位决定 shader 走页表还是 array)
        {
            DescriptorSetLayoutBinding vtB[3] = {
                {14, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // vt_page_table
                {15, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // vt_atlas
                {16, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // vt_params(matBuf 已含 flags)
            };
            (void)vtB;   // 布局已含 0-13;VT 资源经 card_fill matBuf(flags.vtId)
                          // + 全局页表/atlas 绑定接入(VT 系统初始化后)
        }
        card_fill_set_layout_ = device_->CreateDescriptorSetLayout({14, bindings});
    }

    // --- Shadow (T3.3): 3 SDF texture3D + depth/normal atlas reads + params CB
    //     + dispatch/cards SSBOs + visibility write. Vulkan flat / Metal overlap. ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // sdf_cascade_0
            {1, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // sdf_cascade_1
            {2, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // sdf_cascade_2
            {3, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // depth_atlas (read)
            {4, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // normal_atlas (read)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr}, // ShadowParams
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // card_dispatch
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // cards
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // visibility (write)
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[0].is3D = true;
        b[1].is3D = true;
        b[2].is3D = true;
        b[3].readonly = true;
        b[4].readonly = true;
        b[6].readonly = true;
        b[7].readonly = true;
        shadow_set_layout_ = device_->CreateDescriptorSetLayout({9, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // sdf0          [[texture(0)]]
            {1, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // sdf1          [[texture(1)]]
            {2, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // sdf2          [[texture(2)]]
            {3, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // depth_atlas   [[texture(3)]]
            {4, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // normal_atlas  [[texture(4)]]
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr}, // ShadowParams  [[buffer(0)]]
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // card_dispatch [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // cards         [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // visibility    [[buffer(3)]]
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[0].is3D = true;
        b[1].is3D = true;
        b[2].is3D = true;
        b[3].readonly = true;
        b[4].readonly = true;
        b[6].readonly = true;
        b[7].readonly = true;
        shadow_set_layout_ = device_->CreateDescriptorSetLayout({9, bindings});
    }
}

void SurfaceCachePass::CreatePipelines() {
    // --- Shader compilation helper ---
    auto CompileComputeShader = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(device_, name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    auto CompileGraphicsShader = [&](const char* name, const char* entry, ShaderStage stage) -> ShaderHandle {
        auto code = LoadShaderBytecode(device_, name);
        if (code.empty()) return handles::INVALID_SHADER;
        return device_->CreateShader(code.data(), code.size(), stage, entry);
    };

    const bool isVk = device_->GetPlatform() == rhi::RHIPlatform::Vulkan;

    // --- Compile all compute shaders ---
    // Entry names: Metal uses camelCase, Vulkan GLSL uses snake_case.
    auto dilateShader       = CompileComputeShader("SurfaceCacheDilate",
        isVk ? "surface_cache_dilate" : "surfaceCacheDilate");
    auto lightCullShader    = CompileComputeShader("SurfaceCacheLightCull",
        isVk ? "surface_cache_light_cull" : "surfaceCacheLightCull");
    auto lightEvalShader    = CompileComputeShader("SurfaceCacheLightEval",
        isVk ? "surface_cache_light_eval" : "surfaceCacheLightEval");
    auto indirectTraceShader  = CompileComputeShader("SurfaceCacheIndirectTrace",
        isVk ? "surface_cache_indirect_trace" : "surfaceCacheIndirectTrace");
    auto indirectResolveShader = CompileComputeShader("SurfaceCacheIndirectResolve",
        isVk ? "surface_cache_indirect_resolve" : "surfaceCacheIndirectResolve");
    // Atlas default-content init (stands in for Capture coverage gaps).
    // T8.13 sync: compiled on Metal too — heap atlases need the seed.
    ShaderHandle atlasInitShader = handles::INVALID_SHADER;
    {
        atlasInitShader = CompileComputeShader("SurfaceCacheAtlasInit",
            isVk ? "surface_cache_atlas_init" : "surfaceCacheAtlasInit");
    }
    // Vulkan-only: GBuffer-gather capture (real rendered surfaces → card atlas)
    ShaderHandle captureGatherShader = handles::INVALID_SHADER;
    if (isVk) {
        captureGatherShader = CompileComputeShader("SurfaceCacheCapture", "surface_cache_capture");
    }
    // Vulkan-only: SDF-based card fill (all card texels get geometry + lighting)
    ShaderHandle cardFillShader = handles::INVALID_SHADER;
    if (isVk) {
        cardFillShader = CompileComputeShader("SurfaceCacheCardFill", "surface_cache_card_fill");
    }

    // --- Compile capture graphics shaders (Metal only — no Vulkan .vert.spv/.frag.spv yet) ---
    ShaderHandle captureVS = handles::INVALID_SHADER;
    ShaderHandle captureFS = handles::INVALID_SHADER;
    if (!isVk) {
        captureVS = CompileGraphicsShader("SurfaceCacheCapture", "surfaceCacheCaptureVS", ShaderStage::Vertex);
        captureFS = CompileGraphicsShader("SurfaceCacheCapture", "surfaceCacheCaptureFS", ShaderStage::Pixel);
    }

    if (dilateShader == handles::INVALID_SHADER ||
        lightCullShader == handles::INVALID_SHADER ||
        lightEvalShader == handles::INVALID_SHADER ||
        indirectTraceShader == handles::INVALID_SHADER ||
        indirectResolveShader == handles::INVALID_SHADER ||
        (!isVk && (captureVS == handles::INVALID_SHADER ||
                   captureFS == handles::INVALID_SHADER))) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] Shader compilation failed" << std::endl;
        return;
    }

    // --- Create pipeline layouts ---
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &dilate_set_layout_;
        dilate_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &light_cull_set_layout_;
        light_cull_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &light_eval_set_layout_;
        light_eval_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    if (!isVk) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &capture_set_layout_;
        capture_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &indirect_trace_set_layout_;
        indirect_trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &indirect_resolve_set_layout_;
        indirect_resolve_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // --- Create compute pipelines ---
    // Dilate: (8,8,1)
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = dilateShader;
        pipeDesc.layout = dilate_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        dilate_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // LightCull: (8,8,1)
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = lightCullShader;
        pipeDesc.layout = light_cull_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        light_cull_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // LightEval (merged): (256,1,1) — 1D flattened card-texel dispatch
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = lightEvalShader;
        pipeDesc.layout = light_eval_layout_;
        pipeDesc.threadGroupSize = {256, 1, 1};
        light_eval_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // IndirectTrace: (8,8,1)
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = indirectTraceShader;
        pipeDesc.layout = indirect_trace_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        indirect_trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // IndirectResolve: (8,8,1)
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = indirectResolveShader;
        pipeDesc.layout = indirect_resolve_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        indirect_resolve_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // AtlasInit: (256,1,1) — same flattened texel dispatch as LightEval.
    // T8.13 sync: also built on Metal (see layout note above).
    if (atlasInitShader != handles::INVALID_SHADER) {
        {
            PipelineLayoutDesc plDesc;
            plDesc.setLayoutCount = 1;
            plDesc.setLayouts = &atlas_init_set_layout_;
            atlas_init_layout_ = device_->CreatePipelineLayout(plDesc);
        }
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = atlasInitShader;
        pipeDesc.layout = atlas_init_layout_;
        pipeDesc.threadGroupSize = {256, 1, 1};
        atlas_init_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // Capture gather (Vulkan-only): (8,8,1) over render resolution
    if (isVk && captureGatherShader != handles::INVALID_SHADER) {
        {
            PipelineLayoutDesc plDesc;
            plDesc.setLayoutCount = 1;
            plDesc.setLayouts = &capture_gather_set_layout_;
            capture_gather_layout_ = device_->CreatePipelineLayout(plDesc);
        }
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = captureGatherShader;
        pipeDesc.layout = capture_gather_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        capture_gather_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // CardFill (Vulkan-only): (256,1,1) — same flattened texel dispatch as LightEval
    if (isVk && cardFillShader != handles::INVALID_SHADER) {
        {
            PipelineLayoutDesc plDesc;
            plDesc.setLayoutCount = 1;
            plDesc.setLayouts = &card_fill_set_layout_;
            card_fill_layout_ = device_->CreatePipelineLayout(plDesc);
        }
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = cardFillShader;
        pipeDesc.layout = card_fill_layout_;
        pipeDesc.threadGroupSize = {256, 1, 1};
        card_fill_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // Shadow (T3.3): (256,1,1) — per-texel SDF shadow march toward the light
    {
        auto shadowShader = CompileComputeShader("SurfaceCacheShadow",
            isVk ? "surface_cache_shadow" : "surfaceCacheShadow");
        if (shadowShader != handles::INVALID_SHADER) {
            PipelineLayoutDesc plDesc;
            plDesc.setLayoutCount = 1;
            plDesc.setLayouts = &shadow_set_layout_;
            shadow_layout_ = device_->CreatePipelineLayout(plDesc);

            ComputePipelineDesc pipeDesc{};
            pipeDesc.computeShader = shadowShader;
            pipeDesc.layout = shadow_layout_;
            pipeDesc.threadGroupSize = {256, 1, 1};
            shadow_pipeline_ = device_->CreateComputePipeline(pipeDesc);

            for (int i = 0; i < 3; ++i)
                shadow_set_[i] = device_->CreateDescriptorSet({shadow_set_layout_});
        } else {
            PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] Shadow shader compilation failed (shadows disabled)" << std::endl;
        }
    }

    // --- Create capture graphics pipeline (Metal only) ---
    if (!isVk) {
        GraphicsPipelineDesc desc{};
        desc.layout = capture_layout_;
        desc.vertexShader = captureVS;
        desc.pixelShader = captureFS;

        // Vertex attributes: position(0)=float3, normal(1)=float3, uv(2)=float2
        desc.vertexAttributes.clear();
        desc.vertexAttributes.push_back({0, 0, DataFormat::RGB32_Float,  0});  // position @ offset 0
        desc.vertexAttributes.push_back({1, 0, DataFormat::RGB32_Float, 12});  // normal   @ offset 12
        desc.vertexAttributes.push_back({2, 0, DataFormat::RG32_Float,  24});  // uv       @ offset 24

        desc.vertexBindings.clear();
        desc.vertexBindings.push_back({0, 32, true});  // binding 0, stride=32, per-vertex

        desc.topology = PrimitiveTopology::TriangleList;
        desc.cullMode = CullMode::None;

        // 4 render targets matching CaptureOutput struct in shader
        desc.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;    // albedo  [[color(0)]]
        desc.renderTargetFormats[1] = DataFormat::RG16_Float;     // normal  [[color(1)]]
        desc.renderTargetFormats[2] = DataFormat::R32_Float;      // depth   [[color(2)]]
        desc.renderTargetFormats[3] = DataFormat::RGBA16_Float;   // emissive [[color(3)]]
        desc.renderTargetCount = 4;

        desc.depthStencilFormat = DataFormat::Unknown;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;

        capture_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }

    // --- Create triple-buffered descriptor sets ---
    for (int i = 0; i < 3; i++) {
        // T8.13 fix: Metal also creates these sets now (AtlasInit runs on
        // Metal — previously the isVk gate left atlas_init_set_ INVALID on
        // Metal, so the seed never dispatched and the SC stayed black).
        if (atlas_init_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{atlas_init_set_layout_};
            atlas_init_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (isVk && capture_gather_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{capture_gather_set_layout_};
            capture_gather_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (isVk && card_fill_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{card_fill_set_layout_};
            card_fill_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{dilate_set_layout_};
            dilate_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{light_cull_set_layout_};
            light_cull_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{light_eval_set_layout_};
            light_eval_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (!isVk) {
            DescriptorSetDesc dsDesc{capture_set_layout_};
            capture_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{indirect_trace_set_layout_};
            indirect_trace_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{indirect_resolve_set_layout_};
            indirect_resolve_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
    }
}

// ============================================================================
// AddPass — main entry point called per frame
// ============================================================================

// ============================================================================
// T8.12: Card-view capture — renders a rotating batch of cards from their own
// orthographic views directly into the atlas pages. Replaces the camera-bound
// screen-space gather when SC_CARDVIEW=1. Each card entry IS one face (the
// generator allocates per-face), so the cursor walks card indices.
// ============================================================================
void SurfaceCachePass::AddCardCapturePasses(
    RenderGraph& graph,
    const RenderSceneSnapshot& scene_snapshot,
    u32 current_frame_index) {
    if (!card_view_enabled_ || !initialized_) return;

    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    if (!gpuDraw.IsCardCaptureReady() && !gpuDraw.InitializeCardCaptureResources(1u << 20)) return;

    const auto& cards = card_generator_.GetCards();
    const u32 total = card_generator_.GetCardCount();
    if (total == 0) return;

    struct CapPassData {};
    graph.AddPass<CapPassData>(
        "SurfaceCacheCardCapture",
        RGPassType::Graphics,
        RGPassCategory::Lighting,
        [](CapPassData&, RenderGraphBuilder& builder) {
            builder.SideEffect();  // atlases are manually barriered module RTs
        },
        [this, &scene_snapshot, &gpuDraw, total, current_frame_index](const CapPassData&, RenderGraphContext& ctx) {
            auto* cmd = ctx.cmdBuffer;
            const auto& cards = card_generator_.GetCards();

            // Seed batch must stay ≤ kCardCaptureSlots (32): scratch slots are
            // CPU-uploaded at encode time, so folding >32 faces per frame onto
            // 32 slots overwrites data for draws earlier in the same buffer.
            u32 batch = card_view_seeded_ ? 8u : 32u;
            if (const char* b = std::getenv("SC_CARDVIEW_BATCH"))
                batch = (u32)std::max(1, atoi(b));

            // Storage (GENERAL) → color-attachment layouts for the three pages.
            ResourceHandle pages[3] = { albedo_atlas_, normal_atlas_, depth_atlas_ };
            for (auto h : pages) {
                ResourceBarrier b{};
                b.resource = h;
                b.beforeState = ResourceState::UnorderedAccess;
                b.afterState  = ResourceState::RenderTarget;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }

            u32 faces = 0;
            for (u32 k = 0; k < total && faces < batch; ++k) {
                const u32 ci = (card_view_cursor_ + k) % total;
                const auto& card = cards[ci];
                if (card.resolution == 0) continue;

                const uint axis = card.axis_direction & 0xFFu;
                const uint dir  = (card.axis_direction >> 8u) & 0xFFu;

                math::v3 fwd, up;
                float hu = 0.0f, hv = 0.0f, halfAxis = 0.0f;
                if (axis == 0u) {
                    fwd = math::v3{dir > 0u ? 1.0f : -1.0f, 0.0f, 0.0f};
                    up  = math::v3{0.0f, 0.0f, -1.0f};
                    hu = card.extent.y; hv = card.extent.z; halfAxis = card.extent.x;
                } else if (axis == 1u) {
                    fwd = math::v3{0.0f, dir > 0u ? 1.0f : -1.0f, 0.0f};
                    up  = math::v3{0.0f, 0.0f, -1.0f};
                    hu = card.extent.x; hv = card.extent.z; halfAxis = card.extent.y;
                } else {
                    fwd = math::v3{0.0f, 0.0f, dir > 0u ? 1.0f : -1.0f};
                    up  = math::v3{0.0f, -1.0f, 0.0f};
                    hu = card.extent.x; hv = card.extent.y; halfAxis = card.extent.z;
                }
                if (hu <= 0.0f || hv <= 0.0f || halfAxis <= 0.0f) continue;

                const math::v3 center{card.center.x, card.center.y, card.center.z};
                const math::v3 at = center + fwd;
                const math::m4x4 view = rhimath::CreateLookAtMatrix(center, at, up);
                const math::m4x4 proj = rhimath::CreateOrthographicMatrix(
                    -hu, hu, -hv, hv, 0.05f, 2.0f * halfAxis + 0.5f);

                const bool ok = gpuDraw.ExecuteCardCapturePass(
                    cmd, scene_snapshot, view, proj,
                    math::v4{center.x, center.y, center.z, 1.0f},
                    math::v4{fwd.x, fwd.y, fwd.z, 0.0f},
                    0.05f, 2.0f * halfAxis + 0.5f,
                    faces,          // face_slot — unique within this frame
                    current_frame_index,
                    card.atlas_offset_x, card.atlas_offset_y, card.resolution,
                    albedo_atlas_, normal_atlas_, depth_atlas_, card_view_depth_);
                if (ok) ++faces;
            }

            card_view_cursor_ = (card_view_cursor_ + faces) % total;
            if (card_view_cursor_ == 0 && faces > 0) card_view_seeded_ = true;

            // Back to storage layouts for the LightEval/consumer imageLoads.
            for (auto h : pages) {
                ResourceBarrier b{};
                b.resource = h;
                b.beforeState = ResourceState::RenderTarget;
                b.afterState  = ResourceState::UnorderedAccess;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }
        });
}

SurfaceCacheOutput SurfaceCachePass::AddPass(
    RenderGraph& graph,
    RGResourceHandle prev_frame_color,
    ResourceHandle light_data_buffer,
    const SurfaceCacheFrameData& frame_data,
    u32 current_frame_index)
{
    SurfaceCacheOutput output{};

    u32 outIdx = current_frame_index % 3;

    // Import persistent lighting atlas into render graph
    auto lightingHandle = graph.ImportResource(
        "SurfaceCache_Lighting_" + std::to_string(outIdx),
        lighting_atlas_[outIdx]);

    output.lighting_atlas = lightingHandle;

    graph.AddPass<SurfaceCacheRGData>("LumenSurfaceCache",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [prev_frame_color, lightingHandle](
            SurfaceCacheRGData& data, RenderGraphBuilder& builder) {
            // Read previous frame color
            builder.Read(prev_frame_color, ResourceState::ShaderResource);

            data.prev_frame_color = prev_frame_color;

            // Write declaration (prevents RG cull). DEBUG: imported under a
            // side name so the "SurfaceCache_Lighting_N" identity isn't shared.
            builder.Write(lightingHandle, ResourceState::UnorderedAccess);
        },

        // ====================================================================
        // Execute lambda: merged per-card texel lighting dispatch
        // ====================================================================
        [this, frame_data, current_frame_index, outIdx, lightingHandle](
            const SurfaceCacheRGData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            u32 frameIdx = current_frame_index % 3;

            // Guard: pipeline must be valid
            if (light_eval_pipeline_ == handles::INVALID_PIPELINE) return;

            // ================================================================
            // 1. Build CardDispatchInfo from active cards (prefix-sum)
            // ================================================================
            const auto& cards = card_generator_.GetCards();
            u32 card_count = card_generator_.GetCardCount();
            u32 total_texels = 0;

            std::vector<CardDispatchInfo> dispatch_info(card_count);
            for (u32 i = 0; i < card_count; ++i) {
                u32 res = cards[i].resolution;
                u32 count = res * res;
                dispatch_info[i].texel_offset = total_texels;
                dispatch_info[i].texel_count = count;
                dispatch_info[i].resolution = res;
                dispatch_info[i].atlas_offset_x = cards[i].atlas_offset_x;
                dispatch_info[i].atlas_offset_y = cards[i].atlas_offset_y;
                dispatch_info[i].card_index = i;
                memset(dispatch_info[i]._pad, 0, sizeof(dispatch_info[i]._pad));
                total_texels += count;
            }

            // Skip dispatch when no card data
            if (total_texels == 0) return;

            // ================================================================
            // 1.5 (T1.4) Page-budget round-robin selection.
            //
            // Activates surface_cache_capture_budget (was a dead config
            // field): each frame only a rotating window of cards whose texels
            // fit `budget_pages × page_size²` is dispatched, so LightEval
            // invocations drop ~total/budget× instead of lighting every card
            // texel every frame. Un-updated atlas regions keep their previous
            // slot content (persistent-cache semantics); the multi-bounce
            // feedback converges at a proportionally slower rate. budget = 0
            // or budget ≥ total keeps the legacy full update.
            // ================================================================
            {
                // PRIMAL_SC_BUDGET：页预算覆盖（页/帧）。0 = 全量更新——
                // Vulkan 基准行为（实测 374 卡/帧 vs Metal 预算后 8 卡/帧）。
                // SC atlas 覆盖不足曾是 Metal finalize 的 SC-first 层级退化
                // 为"黑+解析蓝"的根因（0c2e34d9 因此错误地改用 prev-first）。
                static const u32 s_budgetOverride = [] {
                    const char* e = std::getenv("PRIMAL_SC_BUDGET");
                    return e ? (u32)std::atoi(e) : 0xFFFFFFFFu;
                }();
                u32 budgetPages = (s_budgetOverride != 0xFFFFFFFFu)
                    ? s_budgetOverride : config_.surface_cache_capture_budget;
                const u32 texel_budget = budgetPages * page_size_ * page_size_;
                // Lighting is triple-buffered and partial updates preserve the
                // untouched texels in each slot. Seed every slot with one full
                // evaluation before page budgeting starts; otherwise each
                // atlas contains only its first tiny card batch and consumers
                // interpret all other, never-written regions as black GI.
                const u32 atlas_seed_bit = 1u << outIdx;
                const bool lighting_atlas_seeded =
                    (lighting_atlas_seed_mask_ & atlas_seed_bit) != 0u;
                if (lighting_atlas_seeded && texel_budget > 0 &&
                    total_texels > texel_budget && card_count > 1) {
                    // Keep the full count for cursor advancement. card_count is
                    // replaced by `taken` below for the GPU dispatch; taking
                    // modulo that reduced count pinned the cursor at zero.
                    const u32 source_card_count = card_count;
                    const u32 start = light_eval_rr_cursor_ % source_card_count;
                    u32 acc = 0, taken = 0;
                    for (u32 k = 0; k < source_card_count; ++k) {
                        const u32 cnt = dispatch_info[(start + k) % source_card_count].texel_count;
                        if (taken > 0 && acc + cnt > texel_budget) break;
                        acc += cnt;
                        ++taken;
                    }
                    if (taken < source_card_count) {
                        std::vector<CardDispatchInfo> selected(taken);
                        u32 off = 0;
                        for (u32 k = 0; k < taken; ++k) {
                            const u32 idx = (start + k) % source_card_count;
                            selected[k] = dispatch_info[idx];
                            selected[k].texel_offset = off;   // rebase to 0
                            off += selected[k].texel_count;
                        }
                        dispatch_info = std::move(selected);
                        card_count = taken;
                        total_texels = off;
                    }
                    light_eval_rr_cursor_ = (start + taken) % source_card_count;
                }
            }

            // Upload dispatch info to GPU
            {
                auto* mapped = static_cast<CardDispatchInfo*>(device_->MapBuffer(card_dispatch_buffer_));
                if (mapped) {
                    memcpy(mapped, dispatch_info.data(), card_count * sizeof(CardDispatchInfo));
                    device_->UnmapBuffer(card_dispatch_buffer_);
                }
            }

            // ================================================================
            // 2. Upload light list into light_info_buffer_ (T3.2: direction/
            //    color come from the caller's frame data — the pipeline
            //    injects settings_.lighting — replacing the hardcoded light).
            //    (LightInfo = position(vec4) + color(vec4) + direction(vec4) = 48B)
            // ================================================================
            {
                struct LightInfoGPU {
                    math::v4 position;    // xyz=pos, w=radius
                    math::v4 color;       // xyz=color, w=unused
                    math::v4 direction;   // xyz=dir, w=type (1.0=directional)
                };
                static_assert(sizeof(LightInfoGPU) == 48, "LightInfo must be 48 bytes");
                auto* lights = static_cast<LightInfoGPU*>(device_->MapBuffer(light_info_buffer_));
                if (lights) {
                    const u32 lightCount = frame_data.light_count > 0 ? frame_data.light_count : 1u;
                    for (u32 i = 0; i < lightCount; ++i) {
                        lights[i].position  = math::v4{0.0f, 0.0f, 0.0f, 1000.0f};
                        lights[i].color     = math::v4{frame_data.light_color.x,
                                                       frame_data.light_color.y,
                                                       frame_data.light_color.z, 0.0f};
                        lights[i].direction = math::v4{frame_data.light_direction.x,
                                                       frame_data.light_direction.y,
                                                       frame_data.light_direction.z, 1.0f}; // w=1 → directional
                    }
                    device_->UnmapBuffer(light_info_buffer_);
                }
            }

            // ================================================================
            // 3. Upload FlattenedLightingParams constant buffer
            // ================================================================
            {
                auto* mapped = static_cast<FlattenedLightingParams*>(device_->MapBuffer(params_cb_[frameIdx]));
                if (mapped) {
                    FlattenedLightingParams params{};
                    params.sc_params.atlas_size = atlas_size_;
                    params.sc_params.page_size = page_size_;
                    params.sc_params.capture_budget_pages = config_.surface_cache_capture_budget;
                    params.sc_params.max_cards = config_.surface_cache_max_cards;
                    params.sc_params.update_distance = config_.surface_cache_update_distance;
                    params.sc_params.importance_weight = config_.surface_cache_importance_weight;
                    params.sc_params.max_lights_per_tile = 32;
                    params.sc_params.indirect_rays_per_probe = 64;
                    params.sc_params.indirect_temporal_weight = 0.9f;
                    params.sc_params.indirect_near_distance = 0.1f;
                    params.sc_params.lookup_count = card_generator_.GetLookupCount();
                    params.total_texels = total_texels;
                    params.card_count = card_count;
                    params.light_count = frame_data.light_count;
                    params._pad = 0;
                    // Probe-fed indirect grid (Lumen hierarchy); valid flag 0
                    // keeps the shader on the legacy self-feedback path.
                    params.ddgi_origin_spacing[0] = frame_data.ddgi_origin.x;
                    params.ddgi_origin_spacing[1] = frame_data.ddgi_origin.y;
                    params.ddgi_origin_spacing[2] = frame_data.ddgi_origin.z;
                    params.ddgi_origin_spacing[3] = frame_data.ddgi_spacing;
                    params.ddgi_counts_valid[0] = frame_data.ddgi_counts[0];
                    params.ddgi_counts_valid[1] = frame_data.ddgi_counts[1];
                    params.ddgi_counts_valid[2] = frame_data.ddgi_counts[2];
                    params.ddgi_counts_valid[3] = frame_data.ddgi_valid;
                    *mapped = params;
                    device_->UnmapBuffer(params_cb_[frameIdx]);
                }
            }

            // ================================================================
            // 3.4 Transition material atlases to GENERAL (first use: UNDEFINED
            //     → GENERAL; subsequent frames this is a no-op barrier).
            //     These textures are not declared in the render graph (only
            //     lighting_atlas is), so their layout must be managed here.
            // ================================================================
            {
                ResourceBarrier atlasBarriers[4]{};
                atlasBarriers[0].resource = albedo_atlas_;
                atlasBarriers[1].resource = normal_atlas_;
                atlasBarriers[2].resource = depth_atlas_;
                atlasBarriers[3].resource = emissive_atlas_;
                for (auto& b : atlasBarriers) {
                    b.beforeState = ResourceState::UnorderedAccess;
                    b.afterState  = ResourceState::UnorderedAccess;
                    b.subresource = 0xFFFFFFFF;
                }
                cmd->InsertBarrier(atlasBarriers, 4);
            }

            // ================================================================
            // 3.5 AtlasInit dispatch (Vulkan-only, FIRST FRAME ONLY): write
            //     default material into card atlas regions so LightEval has
            //     alpha=1 albedo to process even before Capture covers texels.
            // ================================================================
            if (atlas_init_pipeline_ != handles::INVALID_PIPELINE && !atlas_seeded_) {
                static bool s_ai_once = false;
                if (!s_ai_once) {
                    s_ai_once = true;
                    PRIMAL_LOG_STREAM(Error, "Render") << "[ATLAS_INIT] dispatching, pipeline valid" << std::endl;
                }
                // Buffer bindings are platform-numbered (same convention as the
                // Shadow/LightEval dispatch sites): Vulkan layout declares 4/5
                // (flat), Metal layout declares 1/2 to match the kernel's
                // [[buffer(1)]]/[[buffer(2)]]. Writing the wrong numbering
                // silently skips the update (MetalDescriptorSet::Update matches
                // binding+type) — the kernel then reads an unbound buffer
                // (total_texels=0 → every thread early-outs → atlas stays zero).
                const bool aiIsVk = device_->GetPlatform() == rhi::RHIPlatform::Vulkan;
                DescriptorData initParams[] = {
                    {0, DescriptorType::StorageImage,  albedo_atlas_},
                    {1, DescriptorType::StorageImage,  normal_atlas_},
                    {2, DescriptorType::StorageImage,  depth_atlas_},
                    {3, DescriptorType::StorageImage,  emissive_atlas_},
                    {aiIsVk ? 4u : 1u, DescriptorType::UniformBuffer, params_cb_[frameIdx]},
                    {aiIsVk ? 5u : 2u, DescriptorType::StorageBuffer, card_dispatch_buffer_},
                };
                UpdateDescriptorSet(device_, atlas_init_set_[frameIdx], initParams, 6);

                cmd->BindComputePipeline(atlas_init_pipeline_);
                const DescriptorSetHandle initSets[] = { atlas_init_set_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, atlas_init_layout_, 0, 1, initSets, 0, nullptr);

                u32 groups = (total_texels + 255) / 256;
                cmd->Dispatch(groups, 1, 1);
                // T8.13 fix: only mark seeded when there was something to
                // seed. The reordered init creates SurfaceCache before the
                // scene registers cards — the first dispatch then runs with
                // total_texels == 0, and marking seeded there left the atlas
                // unseeded (albedo alpha 0) permanently black.
                if (total_texels > 0) atlas_seeded_ = true;

                // Compute→compute memory barrier (storage images stay in
                // GENERAL layout; LightEval reads via imageLoad).
                cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                   AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
            }

            // ================================================================
            // 3.6 Capture dispatch (Vulkan-only, EVERY FRAME): GBuffer-gather —
            //     scatter real rendered surfaces (albedo / world normal / axis
            //     depth) into card atlas texels. Uncovered texels keep previous
            //     content (persistent atlas = cache semantics).
            // ================================================================
            if (!card_view_enabled_ &&
                capture_gather_pipeline_ != handles::INVALID_PIPELINE &&
                frame_data.gbuffer_depth  != handles::INVALID_RESOURCE &&
                frame_data.gbuffer_albedo != handles::INVALID_RESOURCE &&
                frame_data.gbuffer_normal != handles::INVALID_RESOURCE) {

                // Transition GBuffer inputs to SHADER_READ (first use may be
                // UNDEFINED when the main GBuffer pass hasn't run yet).
                {
                    ResourceBarrier gbBarriers[3]{};
                    gbBarriers[0].resource = frame_data.gbuffer_depth;
                    gbBarriers[1].resource = frame_data.gbuffer_albedo;
                    gbBarriers[2].resource = frame_data.gbuffer_normal;
                    for (auto& b : gbBarriers) {
                        b.beforeState = ResourceState::ShaderResource;
                        b.afterState  = ResourceState::ShaderResource;
                        b.subresource = 0xFFFFFFFF;
                    }
                    cmd->InsertBarrier(gbBarriers, 3);
                }

                // Upload CaptureParams (invViewProj + counts)
                {
                    struct CaptureParamsCB {
                        math::m4x4 invViewProjection;
                        u32 card_count;
                        u32 render_width;
                        u32 render_height;
                        u32 _pad;
                    };
                    auto* mapped = static_cast<CaptureParamsCB*>(device_->MapBuffer(capture_cb_[frameIdx]));
                    if (mapped) {
                        mapped->invViewProjection = frame_data.inv_view_projection;
                        mapped->card_count    = card_count;
                        mapped->render_width  = frame_data.render_width;
                        mapped->render_height = frame_data.render_height;
                        mapped->_pad = 0;
                        device_->UnmapBuffer(capture_cb_[frameIdx]);
                    }
                }

                DescriptorData captureParams[] = {
                    {0, DescriptorType::SampledDepthImage, frame_data.gbuffer_depth},
                    {1, DescriptorType::SampledImage,      frame_data.gbuffer_albedo},
                    {2, DescriptorType::SampledImage,      frame_data.gbuffer_normal},
                    {3, DescriptorType::StorageImage,      albedo_atlas_},
                    {4, DescriptorType::StorageImage,      normal_atlas_},
                    {5, DescriptorType::StorageImage,      depth_atlas_},
                    {6, DescriptorType::UniformBuffer,     capture_cb_[frameIdx]},
                    {7, DescriptorType::StorageBuffer,     card_generator_.GetCardDataBuffer()},
                    {8, DescriptorType::StorageBuffer,     card_generator_.GetGridOffsetsBuffer()},
                    {9, DescriptorType::StorageBuffer,     card_generator_.GetGridCardsBuffer()},
                };
                UpdateDescriptorSet(device_, capture_gather_set_[frameIdx], captureParams, 10);

                cmd->BindComputePipeline(capture_gather_pipeline_);
                const DescriptorSetHandle capSets[] = { capture_gather_set_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, capture_gather_layout_, 0, 1, capSets, 0, nullptr);

                cmd->Dispatch((frame_data.render_width + 7) / 8,
                              (frame_data.render_height + 7) / 8, 1);

                cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                   AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
            }

            // ================================================================
            // 3.7 CardFill dispatch (Vulkan-only, EVERY FRAME): SDF-based —
            //     fills ALL card texels with SDF-derived geometry + lighting.
            //     Runs BEFORE the GBuffer gather's results are consumed by
            //     LightEval, and BEFORE LightEval itself. The GBuffer gather
            //     (which ran above) has already written real materials to
            //     visible texels — CardFill only fills texels the gather
            //     couldn't reach (writing is per-texel, no overwrite of
            //     already-captured data since SDF hit vs no-hit differs).
            //
            //     NOTE: CardFill writes to atlas texels that the gather may
            //     have already written. Since both write the same texel, the
            //     LAST writer wins. CardFill runs after gather, so it would
            //     overwrite gathered material. To avoid this, we SKIP CardFill
            //     for now and only enable it when gather coverage is low.
            //     TODO: Add an albedo-alpha check to skip already-captured texels.
            // ================================================================
            // T3.4 RE-ENABLED (Vulkan): the shader now has the skip guard
            // (non-gray albedo alpha≥0.5 = gather-captured → skip), a
            // min-luminance clamp (no dark patches), and sky fallback —
            // all added after the original disable. CardFill covers texels
            // the screen-space gather can never reach (~0.1% offscreen
            // coverage → SDF-complete coverage of static geometry); its
            // lighting write is transient (LightEval recomputes with real
            // light + T3.3 shadows right after). Metal stays gather-only
            // until a Metal CardFill port lands (M5).
            if (card_fill_pipeline_ != handles::INVALID_PIPELINE) {
                // Get SDF cascade textures + data from GlobalSDF.
                auto& globalSDF = nanite::GlobalSDF::Get();
                ResourceHandle sdfTex[3] = {handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
                u32 cascadeCount = 0;
                if (globalSDF.IsInitialized()) {
                    for (u32 c = 0; c < 3; ++c) {
                        const auto& cascade = globalSDF.GetCascade(c);
                        if (cascade.sdf_texture != handles::INVALID_RESOURCE) {
                            sdfTex[c] = cascade.sdf_texture;
                            cascadeCount++;
                        }
                    }
                }

                if (cascadeCount > 0) {
                    // Upload CardFillParams (SDF cascades + light + counts)
                    {
                        struct CardFillParamsCB {
                            math::v4 sdf_origins[3];
                            math::v4 sdf_extents[3];
                            u32 sdf_res[3];
                            u32 sdf_count;
                            math::v4 light_direction;
                            math::v4 light_color;
                            u32 total_texels;
                            u32 card_count;
                            u32 _pad0;
                            u32 _pad1;
                        };
                        auto* cfm = static_cast<CardFillParamsCB*>(device_->MapBuffer(card_fill_cb_[frameIdx]));
                        if (cfm) {
                            for (u32 c = 0; c < cascadeCount; ++c) {
                                const auto& cascade = globalSDF.GetCascade(c);
                                cfm->sdf_origins[c] = {cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f};
                                cfm->sdf_extents[c] = {cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f};
                                cfm->sdf_res[c] = cascade.resolution;
                            }
                            cfm->sdf_count = cascadeCount;
                            cfm->light_direction = math::v4{0.707f, -1.0f, 0.408f, 0.0f};
                            cfm->light_color = math::v4{5.0f, 5.0f, 5.0f, 1.0f};
                            cfm->total_texels = total_texels;
                            cfm->card_count = card_count;
                            cfm->_pad0 = cfm->_pad1 = 0;
                            device_->UnmapBuffer(card_fill_cb_[frameIdx]);
                        }
                    }

                    // Get instance + material + texture buffers for per-mesh color
                    auto& gpuDrawCF = nanite::GPUDrivenDrawPipeline::Get();
                    ResourceHandle instBuf = gpuDrawCF.GetGlobalInstanceDataBuffer();
                    ResourceHandle matBuf  = gpuDrawCF.GetMaterialDataBuffer();
                    ResourceHandle albedoTexArr = gpuDrawCF.GetAlbedoTextureArray();
                    // Use a simple linear sampler (create if not cached)
                    static SamplerHandle s_cf_sampler = handles::INVALID_SAMPLER;
                    if (s_cf_sampler == handles::INVALID_SAMPLER) {
                        SamplerDesc sd{};
                        sd.minFilter = FilterMode::Linear;
                        sd.magFilter = FilterMode::Linear;
                        sd.addressU = TextureAddressMode::Clamp;
                        sd.addressV = TextureAddressMode::Clamp;
                        s_cf_sampler = device_->CreateSampler(sd);
                    }

                    DescriptorData fillParams[] = {
                        {0, DescriptorType::SampledImage,  sdfTex[0]},
                        {1, DescriptorType::SampledImage,  sdfTex[1]},
                        {2, DescriptorType::SampledImage,  sdfTex[2]},
                        {3, DescriptorType::StorageImage,  albedo_atlas_},
                        {4, DescriptorType::StorageImage,  normal_atlas_},
                        {5, DescriptorType::StorageImage,  depth_atlas_},
                        {6, DescriptorType::StorageImage,  lighting_atlas_[outIdx]},
                        {7, DescriptorType::UniformBuffer, card_fill_cb_[frameIdx]},
                        {8, DescriptorType::StorageBuffer, card_dispatch_buffer_},
                        {9, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()},
                        {10, DescriptorType::StorageBuffer, instBuf},
                        {11, DescriptorType::StorageBuffer, matBuf},
                        {12, DescriptorType::SampledImage,  albedoTexArr},
                        {13, DescriptorType::Sampler,       static_cast<ResourceHandle>(s_cf_sampler)},
                    };
                    UpdateDescriptorSet(device_, card_fill_set_[frameIdx], fillParams, 14);

                    cmd->BindComputePipeline(card_fill_pipeline_);
                    const DescriptorSetHandle fillSets[] = { card_fill_set_[frameIdx] };
                    cmd->BindDescriptorSets(PipelineBindPoint::Compute, card_fill_layout_, 0, 1, fillSets, 0, nullptr);

                    u32 fillGroups = (total_texels + 255) / 256;
                    cmd->Dispatch(fillGroups, 1, 1);

                    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                       AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
                }
            }

            // ================================================================
            // 3.8 (T3.3) Shadow dispatch — per-texel SDF march toward the light.
            // Split-pass by read type: texture3D-heavy shadow first, then the
            // buffer/image-only LightEval consumes the visibility buffer.
            // Skipped when GlobalSDF is unavailable — visibility stays 1.0.
            // ================================================================
            {
                auto& globalSDF = nanite::GlobalSDF::Get();
                if (shadow_pipeline_ != handles::INVALID_PIPELINE &&
                    shadow_visibility_buffer_ != handles::INVALID_RESOURCE &&
                    globalSDF.IsInitialized()) {
                    static bool s_scshadow_once = false;
                    if (!s_scshadow_once) {
                        s_scshadow_once = true;
                        PRIMAL_LOG_STREAM(Error, "Render") << "[SC_SHADOW] dispatching" << std::endl;
                    }

                    ResourceHandle sdfTex[3] = {
                        handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
                    u32 cascadeCount = 0;
                    // T8.13: prefer the offline merged SDF (384^3, sharp) —
                    // same source as the DDGI trace; runtime cascades stay as
                    // the fallback when no offline bake is wired.
                    if (sc_offline_sdf_texture_ != handles::INVALID_RESOURCE) {
                        sdfTex[0] = sc_offline_sdf_texture_;
                        cascadeCount = 1;
                    } else {
                        for (u32 c = 0; c < 3; ++c) {
                            const auto& cascade = globalSDF.GetCascade(c);
                            if (cascade.sdf_texture != handles::INVALID_RESOURCE) {
                                sdfTex[c] = cascade.sdf_texture;
                                ++cascadeCount;
                            }
                        }
                    }

                    if (cascadeCount > 0) {
                        const bool shIsVk =
                            device_->GetPlatform() == rhi::RHIPlatform::Vulkan;

                        // ShadowParams (mirror of the shader UBO, 96B)
                        struct ShadowParamsGPU {
                            math::v4 sdf_origins[3];
                            math::v4 sdf_extents[3];
                            math::v4 sdf_res_count;
                            math::v4 light_dir_toward;
                            math::v4 shadow_params;   // x=maxDist, y=bias, z=atlasSize
                            math::v4 counts;          // x=total_texels, y=card_count
                        };
                        auto* sp = static_cast<ShadowParamsGPU*>(device_->MapBuffer(shadow_cb_[frameIdx]));
                        if (sp) {
                            memset(sp, 0, sizeof(ShadowParamsGPU));
                            float resArr[3] = {0.0f, 0.0f, 0.0f};
                            if (cascadeCount == 1 &&
                                sc_offline_sdf_texture_ != handles::INVALID_RESOURCE) {
                                sp->sdf_origins[0] = {sc_offline_sdf_origin_.x, sc_offline_sdf_origin_.y,
                                                      sc_offline_sdf_origin_.z, 0.0f};
                                sp->sdf_extents[0] = {sc_offline_sdf_extent_.x, sc_offline_sdf_extent_.y,
                                                      sc_offline_sdf_extent_.z, 0.0f};
                                resArr[0] = (float)sc_offline_sdf_resolution_;
                            } else {
                                for (u32 c = 0; c < cascadeCount; ++c) {
                                    const auto& cascade = globalSDF.GetCascade(c);
                                    sp->sdf_origins[c] = {cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f};
                                    sp->sdf_extents[c] = {cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f};
                                    resArr[c] = (float)cascade.resolution;
                                }
                            }
                            sp->sdf_res_count = {resArr[0], resArr[1], resArr[2], (float)cascadeCount};
                            math::v3 ld = frame_data.light_direction;
                            float ldLen = std::sqrt(ld.x * ld.x + ld.y * ld.y + ld.z * ld.z);
                            if (ldLen < 1e-5f) ldLen = 1.0f;
                            math::v3 toward = {-ld.x / ldLen, -ld.y / ldLen, -ld.z / ldLen};
                            sp->light_dir_toward = {toward.x, toward.y, toward.z, 0.0f};
                            sp->shadow_params = {50.0f, 1.5f, (float)atlas_size_, 0.0f};
                            sp->counts = {(float)total_texels, (float)card_count, 0.0f, 0.0f};
                            device_->UnmapBuffer(shadow_cb_[frameIdx]);
                        }

                        DescriptorData shadowParams[] = {
                            {0, DescriptorType::SampledImage, sdfTex[0]},
                            {1, DescriptorType::SampledImage, sdfTex[1]},
                            {2, DescriptorType::SampledImage, sdfTex[2]},
                            {3, DescriptorType::StorageImage, depth_atlas_},
                            {4, DescriptorType::StorageImage, normal_atlas_},
                            {shIsVk ? 5u : 0u, DescriptorType::UniformBuffer, shadow_cb_[frameIdx]},
                            {shIsVk ? 6u : 1u, DescriptorType::StorageBuffer, card_dispatch_buffer_},
                            {shIsVk ? 7u : 2u, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()},
                            {shIsVk ? 8u : 3u, DescriptorType::StorageBuffer, shadow_visibility_buffer_},
                        };
                        UpdateDescriptorSet(device_, shadow_set_[frameIdx], shadowParams, 9);

                        cmd->BindComputePipeline(shadow_pipeline_);
                        const DescriptorSetHandle shSets[] = { shadow_set_[frameIdx] };
                        cmd->BindDescriptorSets(PipelineBindPoint::Compute, shadow_layout_, 0, 1, shSets, 0, nullptr);

                        u32 shGroups = (total_texels + 255) / 256;
                        cmd->Dispatch(shGroups, 1, 1);

                        // visibility write → LightEval read
                        cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                           AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
                    }
                }
            }

            // ================================================================
            // 4. Single merged LightEval dispatch (1D, per-card texel)
            // ================================================================
            {
                const bool isVk = device_->GetPlatform() == rhi::RHIPlatform::Vulkan;
                // histIdx = 2 frames ago — the lighting atlas from 2 frames
                // back is safe to read (GPU finished with it) and serves as
                // the multi-bounce feedback source.
                u32 histIdx = (current_frame_index + 1) % 3;
                DescriptorData evalParams[12];
                if (isVk) {
                    DescriptorData p[] = {
                        {0, DescriptorType::StorageImage,  albedo_atlas_},
                        {1, DescriptorType::StorageImage,  normal_atlas_},
                        {2, DescriptorType::StorageImage,  emissive_atlas_},
                        {3, DescriptorType::StorageImage,  lighting_atlas_[outIdx]},
                        {4, DescriptorType::UniformBuffer, params_cb_[frameIdx]},
                        {5, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()},
                        {6, DescriptorType::StorageBuffer, light_info_buffer_},
                        {7, DescriptorType::StorageBuffer, card_dispatch_buffer_},
                        {8, DescriptorType::StorageImage,  lighting_atlas_[histIdx]},  // prev for multi-bounce
                        {9, DescriptorType::StorageBuffer, shadow_visibility_buffer_},  // T3.3
                        // Probe-fed indirect: bind the previous-frame slot
                        // even when invalid (CB flag gates the shader term).
                        {10, DescriptorType::StorageBuffer,
                         frame_data.ddgi_irradiance != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_irradiance
                             : card_dispatch_buffer_},
                        // Surface-point reconstruction for the probe gather
                        // (same depth source the shadow pass uses).
                        {11, DescriptorType::StorageImage,  depth_atlas_},
                    };
                    memcpy(evalParams, p, sizeof(p));
                } else {
                    // T8.13 Vulkan↔Metal sync: same 12-resource set as the
                    // Vulkan branch — Metal slot indices differ only by
                    // namespace (textures 0-5, buffers 1-6).
                    DescriptorData p[] = {
                        {0, DescriptorType::StorageImage,  albedo_atlas_},
                        {1, DescriptorType::StorageImage,  normal_atlas_},
                        {2, DescriptorType::StorageImage,  emissive_atlas_},
                        {3, DescriptorType::StorageImage,  lighting_atlas_[outIdx]},
                        {1, DescriptorType::UniformBuffer, params_cb_[frameIdx]},
                        {2, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()},
                        {3, DescriptorType::StorageBuffer, light_info_buffer_},
                        {4, DescriptorType::StorageBuffer, card_dispatch_buffer_},
                        {5, DescriptorType::StorageBuffer, shadow_visibility_buffer_},  // T3.3
                        {4, DescriptorType::StorageImage,  lighting_atlas_[histIdx]},   // prev for multi-bounce
                        {5, DescriptorType::StorageImage,  depth_atlas_},               // surface-point probe gather
                        // Probe-fed indirect: bind the previous-frame slot even
                        // when invalid (CB flag gates the shader term).
                        {6, DescriptorType::StorageBuffer,
                         frame_data.ddgi_irradiance != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_irradiance
                             : card_dispatch_buffer_},
                    };
                    memcpy(evalParams, p, sizeof(p));
                }
                UpdateDescriptorSet(device_, light_eval_set_[frameIdx], evalParams, 12);

                // Explicit GENERAL transition for the lighting atlases — do not
                // rely on the render graph's Write declaration having put them
                // in a storage-compatible layout before this dispatch. Both the
                // written slot (outIdx) and the multi-bounce feedback slot
                // (histIdx) are bound as storage images; histIdx is not RG-
                // declared, and on its first frames it is still UNDEFINED.
                // The Vulkan backend resolves oldLayout from the tracked layout,
                // so the first such barrier correctly performs UNDEFINED→UAV.
                {
                    ResourceBarrier lb[2]{};
                    lb[0].resource = lighting_atlas_[outIdx];
                    lb[1].resource = lighting_atlas_[histIdx];
                    for (auto& b : lb) {
                        b.beforeState = ResourceState::UnorderedAccess;
                        b.afterState  = ResourceState::UnorderedAccess;
                        b.subresource = 0xFFFFFFFF;
                    }
                    cmd->InsertBarrier(lb, 2);
                }

                cmd->BindComputePipeline(light_eval_pipeline_);
                const DescriptorSetHandle sets[] = { light_eval_set_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, light_eval_layout_, 0, 1, sets, 0, nullptr);

                u32 groups = (total_texels + 255) / 256;
                cmd->Dispatch(groups, 1, 1);

                // Mark this slot as holding one full evaluation (the budget
                // block above only clips slots that already passed through
                // here once). AtlasInit sets atlas_seeded_ on frame 0 for
                // slot 0 only; slots 1/2 rotate in on frames 1/2 and must
                // seed themselves the same way.
                lighting_atlas_seed_mask_ |= 1u << outIdx;

                // DEBUG: immediate post-dispatch memory barrier + inline readback
                // to prove/disprove whether the imageStore landed in this very
                // command buffer (rules out later-pass overwrite).
                PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] LightEval dispatch: " << total_texels
                          << " texels, " << card_count << " cards, "
                          << groups << " groups" << std::endl;

                // TEMP diagnosis: two-shot readback — albedo/normal seed-match
                // rate at an early and a late frame splits "capture never
                // wrote" (seed stays 100%) from "capture progresses".
                {
                    static const u32 rbFrames[2] = { 60, 1200 };
                    for (u32 rbI = 0; rbI < 2; ++rbI) {
                        if (std::getenv("SC_DUMP_CARDS") && current_frame_index == rbFrames[rbI] &&
                            s_albedo_readback_done_ == (rbI == 0)) {
                            s_albedo_readback_done_ = true;
                            struct RB { u64 size; rhi::ResourceHandle tex; const char* name; u32 bpp; };
                            RB rbs[2] = {
                                { (u64)atlas_size_ * atlas_size_ * 4, albedo_atlas_, "albedo(rgba8)", 4 },
                                { (u64)atlas_size_ * atlas_size_ * 4, normal_atlas_, "normal(rg16f)", 4 },
                            };
                            for (auto& rb : rbs) {
                                rhi::BufferDesc d{};
                                d.size = rb.size;
                                d.bindFlags = (u32)rhi::BufferUsageFlags::TransferDst;
                                d.memoryUsage = rhi::GPUMemoryUsage::Readback;
                                d.name = "SC_RB";
                                auto stage = device_->CreateBuffer(d);
                                if (stage == handles::INVALID_RESOURCE) continue;
                                auto ch = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
                                auto* c2 = rhi::GetCommandBuffer(ch);
                                if (!c2) { device_->DestroyBuffer(stage); continue; }
                                c2->Begin();
                                rhi::BufferTextureCopyRegion rr{};
                                rr.imageSubresource.mipLevel = 0;
                                rr.imageSubresource.layerCount = 1;
                                rr.imageExtent = {atlas_size_, atlas_size_, 1};
                                c2->CopyTextureToBuffer(rb.tex, stage, &rr, 1);
                                c2->End(); c2->Submit(); c2->WaitForCompletion();
                                device_->DestroyCommandBuffer(ch);
                                if (auto* p = static_cast<const u8*>(device_->MapBuffer(stage, 0, rb.size))) {
                                    u64 alpha0 = 0, seed = 0, total = (u64)atlas_size_ * atlas_size_;
                                    for (u64 i = 0; i < total; ++i) {
                                        const u8* t = p + i * 4;
                                        if (rb.bpp == 4 && rb.name[0] == 'a') {
                                            // albedo rgba8: alpha gate + exact seed match
                                            if (t[3] == 0) ++alpha0;
                                            if (t[0] == 128 && t[1] == 128 && t[2] == 128 && t[3] == 255) ++seed;
                                        } else {
                                            // normal rg16f: seed = half 0.5 = 0x3800 both channels
                                            u16 r, g;
                                            std::memcpy(&r, t, 2);
                                            std::memcpy(&g, t + 2, 2);
                                            if (r == 0x3800 && g == 0x3800) ++seed;
                                        }
                                    }
                                    PRIMAL_LOG_STREAM(Error, "Render") << "[SC_RB] f" << rbFrames[rbI] << " " << rb.name
                                              << ": alpha0=" << alpha0
                                              << " seed_exact=" << seed
                                              << " non_seed=" << (total - seed)
                                              << " (of " << total << ")" << std::endl;
                                    device_->UnmapBuffer(stage);
                                }
                                device_->DestroyBuffer(stage);
                            }
                        }
                    }
                }
            }

            // ================================================================
            // 4. Barrier: lighting_atlas_ UAV -> SRV
            // ================================================================
            {
                ResourceBarrier barrier{};
                barrier.resource = lighting_atlas_[outIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }
        }
    );

    return output;
}

} // namespace primal::graphics::lumen
