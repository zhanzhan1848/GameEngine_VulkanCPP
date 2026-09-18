#include "SurfaceCachePass.h"
#include "Engine/Graphics/Lumen/GIWarmup.h"
#include "Engine/Graphics/Lumen/DDGISlotMap.h"
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
#include <cstdio>
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
    // C 系列归因旋钮（编译期诊断变体，默认关闭）：SC 侧注入路径逐条断电。
    if (std::getenv("PRIMAL_SC_NO_EMISSIVE")) {
        resolved = "#define PRIMAL_SC_NO_EMISSIVE 1\n" + resolved;
    }
    if (std::getenv("PRIMAL_SC_NO_DIRECT")) {
        resolved = "#define PRIMAL_SC_NO_DIRECT 1\n" + resolved;
    }
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

// G (plan §三.G): the tile-radiosity tile size is resolved ONCE per session.
// The ray_hits buffer cannot follow a mid-run env change without a rebuild,
// so the config must be stable for the buffers' lifetime. Values are rounded
// DOWN to a power of two within [4, 128]: a power-of-two tile always divides
// the power-of-two atlas exactly (no untraced edge strips, exact grid for the
// resolve dispatch and the validity bitmask).
static u32 SCRadiosityTileSize() {
    static const u32 v = [] {
        const char* e = std::getenv("PRIMAL_SC_RADIOSITY_TILE");
        u32 t = e ? (u32)std::atoi(e) : 8u;   // 2026-09-11 promoted: 0.85 m patches
        if (t < 4u) t = 4u;
        if (t > 128u) t = 128u;
        if ((t & (t - 1u)) != 0u) {
            u32 p = 4u;
            while ((p << 1) <= t) p <<= 1;
            t = p;
        }
        return t;
    }();
    return v;
}

float SurfaceCachePass::DebugShadowVisibilityAt(u32 index) const {
    if (shadow_visibility_buffer_ == handles::INVALID_RESOURCE || !device_) return -1.0f;
    auto* p = static_cast<const float*>(device_->MapBuffer(shadow_visibility_buffer_));
    if (!p) return -2.0f;
    float v = p[index];
    device_->UnmapBuffer(shadow_visibility_buffer_);
    return v;
}

void SurfaceCachePass::DebugShadowVisibilityStats() const {
    if (shadow_visibility_buffer_ == handles::INVALID_RESOURCE || !device_) return;
    const u64 n = static_cast<u64>(atlas_size_) * atlas_size_;
    auto* p = static_cast<const float*>(device_->MapBuffer(shadow_visibility_buffer_, 0, n * sizeof(float)));
    if (!p) return;
    double sum = 0.0; u64 zeros = 0; u64 ones = 0; float mx = 0.0f;
    for (u64 i = 0; i < n; ++i) {
        const float v = p[i];
        sum += v;
        if (v <= 1e-4f) ++zeros;
        if (v >= 0.999f) ++ones;
        if (v > mx) mx = v;
    }
    device_->UnmapBuffer(shadow_visibility_buffer_);
    PRIMAL_LOG_STREAM(Error, "Render") << "[VIS_STATS] texels=" << n
              << " mean=" << sum / n
              << " zeros=" << 100.0 * zeros / n << "%"
              << " ones=" << 100.0 * ones / n << "%"
              << " max=" << mx << std::endl;

    // C 系列诊断：atlas 可见性整图落盘（PGM，白=lit/黑=shadowed）。与卡片
    // atlas_offset 对照即可定位"室内 lit"texel 属于哪些卡片。
    if (std::getenv("SC_VIS_DUMP")) {
        auto* q = static_cast<const float*>(device_->MapBuffer(shadow_visibility_buffer_, 0, n * sizeof(float)));
        if (q) {
            std::ofstream out("/tmp/sc_visibility.pgm", std::ios::binary);
            out << "P5\n" << atlas_size_ << " " << atlas_size_ << "\n255\n";
            std::vector<unsigned char> row(atlas_size_);
            for (u32 y = 0; y < atlas_size_; ++y) {
                for (u32 x = 0; x < atlas_size_; ++x)
                    row[x] = (unsigned char)(q[(u64)y * atlas_size_ + x] * 255.0f + 0.5f);
                out.write(reinterpret_cast<const char*>(row.data()), atlas_size_);
            }
            out.close();

            // 每卡片 lit 占比 + 世界坐标关联表 → /tmp/sc_cards_vis.txt。
            // 列：idx atlas_x atlas_y res lit% cx cy cz ex ey ez axis dir。
            std::ofstream cards("/tmp/sc_cards_vis.txt");
            const auto& cardList = card_generator_.GetCards();
            u32 count = std::min<u32>(cardList.size(), 4096u);
            for (u32 ci = 0; ci < count; ++ci) {
                const auto& c = cardList[ci];
                u64 lit = 0, tot = 0;
                for (u32 y = 0; y < c.resolution; ++y) {
                    const u64 rowBase = (u64)(c.atlas_offset_y + y) * atlas_size_;
                    for (u32 x = 0; x < c.resolution; ++x) {
                        float depthChk = 1.0f; // lit 统计含全部 texel（empty texel 也计）
                        (void)depthChk;
                        float v = q[rowBase + c.atlas_offset_x + x];
                        ++tot;
                        if (v >= 0.5f) ++lit;
                    }
                }
                cards << ci << ' ' << c.atlas_offset_x << ' ' << c.atlas_offset_y << ' '
                      << c.resolution << ' '
                      << (tot ? 100.0 * lit / tot : 0.0) << ' '
                      << c.center.x << ' ' << c.center.y << ' ' << c.center.z << ' '
                      << c.extent.x << ' ' << c.extent.y << ' ' << c.extent.z << ' '
                      << (c.axis_direction & 0xFFu) << ' ' << ((c.axis_direction >> 8) & 0xFFu)
                      << '\n';
            }
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[VIS_DUMP] wrote /tmp/sc_visibility.pgm + /tmp/sc_cards_vis.txt ("
                << count << " cards)" << std::endl;
            device_->UnmapBuffer(shadow_visibility_buffer_);
        }
    }
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

// T4.4: per-instance emissive（a>0.5 覆盖 PRIMAL_SC_EMISSIVE 场景级源）。
// Direct mapped 写入（Dynamic host-visible）；capture 下一帧散射进
// emissive_atlas，经 LightEval 的 8.0 钳制进入 SC↔probe 反弹。
void SurfaceCachePass::SetInstanceEmissive(uint32_t instance_id, const math::v3& rgb) {
    if (instance_id >= kMaxEmissiveInstances) {
        PRIMAL_LOG_STREAM(Warn, "Render") << "[SurfaceCache] SetInstanceEmissive: instance "
                  << instance_id << " out of range (max " << kMaxEmissiveInstances << ")" << std::endl;
        return;
    }
    if (instance_emissive_buffer_ == handles::INVALID_RESOURCE) return;
    auto* m = static_cast<math::v4*>(device_->MapBuffer(instance_emissive_buffer_));
    if (m) {
        m[instance_id] = math::v4{rgb.x, rgb.y, rgb.z, 1.0f};
        device_->UnmapBuffer(instance_emissive_buffer_);
    }
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
    // [WRC 计划 Stream D2]（PRIMAL_SC_PAGE_TABLE=1）: 页表间接化分配——
    // 卡片分配记账页坐标 + LRU 挤页（D3）；默认 0 = 行为不变。
    if (const char* ptEnv = std::getenv("PRIMAL_SC_PAGE_TABLE"); ptEnv && ptEnv[0] == '1') {
        card_generator_.EnablePageTable();
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[SurfaceCache] PRIMAL_SC_PAGE_TABLE=1 — page-table allocation "
            << "+ LRU squeeze enabled" << std::endl;
    }

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
        // GI 根治 Phase 3 修（VUID-01211）：该深度也被 LightEval gather 以
        // SRO 采样（卡片可见性平面一致性测试）——usage 必须含 SAMPLED，
        // 否则 RG 的 UNDEF→SRO barrier 对纯 DEPTH_STENCIL usage 非法。
        dv.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
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

    if (device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
        // A7: nearest-depth scratch for the GBuffer-gather capture — one u32
        // (floatBitsToUint of axis depth) per atlas texel. Cleared and
        // consumed on the GPU every frame the capture dispatches.
        BufferDesc capMinDesc{};
        capMinDesc.size = (u64)atlas_size_ * atlas_size_ * sizeof(u32);
        capMinDesc.type = BufferType::Structured;
        capture_min_depth_buffer_ = device_->CreateBuffer(capMinDesc);
    }

    // T5.5 (F5): ray_hits 67MB deferred — allocated lazily on first
    // SC_INDIRECT dispatch (see indirect block in AddPass).

    {
        // Card dispatch buffer: flattened per-card texel dispatch info
        BufferDesc desc{};
        desc.size = (u64)config_.surface_cache_max_cards * sizeof(CardDispatchInfo);
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        card_dispatch_buffer_ = device_->CreateBuffer(desc);
    }

    // T3.3 (D10): indirect trace/resolve wiring resources
    {
        for (int i = 0; i < 3; ++i) {
            BufferDesc cbDesc{};
            cbDesc.size = 288;   // sizeof(IndirectTraceParamsGPU)
            // 按 UniformBuffer 描述符绑定(:2513 等),创建必须 Constant——
            // Structured 只产 STORAGE usage,触发 VUID-00330。
            cbDesc.type = BufferType::Constant;
            cbDesc.usage = GPUMemoryUsage::Dynamic;
            cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            indirect_trace_params_cb_[i] = device_->CreateBuffer(cbDesc);

            cbDesc.size = 96;    // sizeof(IndirectResolveParamsGPU) padded (88 → std140 round-up)
            indirect_resolve_params_cb_[i] = device_->CreateBuffer(cbDesc);

            TextureDesc outDesc{};
            // Resolve expands each tile into material-weighted atlas texels.
            // Allocation and consumers must use this same full-atlas domain.
            outDesc.size = { atlas_size_, atlas_size_, 1 };
            outDesc.mipLevels = 1;
            outDesc.arraySize = 1;
            outDesc.format = DataFormat::RGBA16_Float;
            outDesc.type = TextureType::Texture2D;
            outDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            indirect_out_tex_[i] = device_->CreateTexture(outDesc);

            // A (plan §三.A): per-tile incident radiance + validity (rgb, a)
            // triple-buffered for the tile-domain EMA, plus a single-buffered
            // compat-evidence texture (oct16 normal / capture depth / owning
            // card id) rewritten every resolve dispatch.
            const u32 tileDim = std::min<u32>(SCRadiosityTileSize(), atlas_size_);
            const u32 tilesPerSide = atlas_size_ / tileDim;
            TextureDesc tileDesc = outDesc;
            tileDesc.size = { tilesPerSide, tilesPerSide, 1 };
            indirect_tile_tex_[i] = device_->CreateTexture(tileDesc);
        }
        {
            const u32 tileDim = std::min<u32>(SCRadiosityTileSize(), atlas_size_);
            const u32 tilesPerSide = atlas_size_ / tileDim;
            TextureDesc auxDesc{};
            auxDesc.size = { tilesPerSide, tilesPerSide, 1 };
            auxDesc.mipLevels = 1;
            auxDesc.arraySize = 1;
            auxDesc.format = DataFormat::RGBA16_Float;
            auxDesc.type = TextureType::Texture2D;
            auxDesc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            auxDesc.name = "SurfaceCache_TileAux";
            indirect_tile_aux_ = device_->CreateTexture(auxDesc);
            // P0-B 注：此处不可 prime——tile_aux 的帧内循环是
            // "resolve 写(UA) → recon 采(SRO) → 回置 UA"，tracked 从 UNDEF
            // 起步由首个 recon barrier 正确初始化；prime 到 SRO 反而让
            // resolve 的 storage 写落在 SRO 上（validation 钉 GENERAL）。
        }

        BufferDesc dummyDesc{};
        dummyDesc.size = 16;
        // 按 UniformBuffer 描述符绑定(:2514 等),同上需 Constant 产出 UNIFORM usage。
        dummyDesc.type = BufferType::Constant;
        indirect_dummy_cb_ = device_->CreateBuffer(dummyDesc);

        // P0-B: unused SampledImage slots in the resolve/reconstruct layouts
        // need a binding that is not a live atlas (see header note).
        TextureDesc dummyTexDesc{};
        dummyTexDesc.size = {1, 1, 1};
        dummyTexDesc.format = DataFormat::RGBA16_Float;
        dummyTexDesc.type = TextureType::Texture2D;
        dummyTexDesc.usage = TextureUsage::ShaderResource;
        dummyTexDesc.name = "SurfaceCache_DummySRV";
        indirect_dummy_tex_ = device_->CreateTexture(dummyTexDesc);
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
    CreateCBs(capture_emissive_cb_, 16);  // T4.4: CaptureEmissive (rgba, 16B)

    // T4.4: per-instance emissive 表（1024 × vec4，零初始化 = 无 emissive）。
    if (instance_emissive_buffer_ == handles::INVALID_RESOURCE) {
        BufferDesc ieDesc{};
        ieDesc.size = (u64)kMaxEmissiveInstances * 16;
        ieDesc.type = BufferType::Structured;
        ieDesc.usage = GPUMemoryUsage::Dynamic;
        ieDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        ieDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        ieDesc.structured.elementCount = kMaxEmissiveInstances;
        ieDesc.structured.elementStride = 16;
        instance_emissive_buffer_ = device_->CreateBuffer(ieDesc);
        if (auto* m = static_cast<math::v4*>(device_->MapBuffer(instance_emissive_buffer_))) {
            memset(m, 0, (size_t)kMaxEmissiveInstances * 16);
            device_->UnmapBuffer(instance_emissive_buffer_);
        }
    }
    CreateCBs(card_fill_cb_, 176); // CardFillParams: SDF cascades + light + counts
    CreateCBs(shadow_cb_, 464);    // T3.3/G2: ShadowParams（96B 基础 + 256B shadow 矩阵 + 16B meta）

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
            {12, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // indirect_out (T3.3 tile radiosity)
            {13, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // ddgi depth (Chebyshev gather visibility, C 系列)
            {14, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // GI 根治 Phase 3.2: ddgi openSky (天空分类)
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[9].readonly = true;
        b[10].readonly = true;
        b[13].readonly = true;
        b[14].readonly = true;
        light_eval_set_layout_ = device_->CreateDescriptorSetLayout({15, bindings});
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
            {6, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // indirect_out [[texture(6)]] (T3.3 tile radiosity)
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ddgi depth [[buffer(7)]] (Chebyshev gather visibility, C 系列)
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // GI 根治 Phase 3.2: ddgi openSky [[buffer(8)]] (天空分类)
        };
        DescriptorSetLayoutBinding* bm = bindings;
        bm[5].readonly = true;
        bm[8].readonly = true;  // ddgi irradiance
        bm[13].readonly = true; // ddgi depth
        bm[14].readonly = true; // GI 根治 Phase 3.2: ddgi openSky
        // 修复：15 个条目（含 openSky buffer(8)）但 count 传 14——最后一个
        // 条目从未进布局，Metal LightEval 编码期断言
        // "missing Buffer binding at index 8 for ddgi_opensky_buffer"。
        DescriptorSetLayoutDesc layoutDesc{15, bindings};
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

    // --- IndirectTrace: 5 textures (depth+normal+3 SDF) + 3 SDF mips + 1 UBO + 1 UBO + 2 SSBO ---
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
            // T3.2 (D7) cone-hit 消费者（T3.3 Radiosity 的 indirect trace）：
            // cascade-0 conservative min-mip 链（DispatchMipChain 产出）。
            // 缺槽回绑 sdf0 本体——shader 用 textureSize 对比自门控。
            {9,  DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // cascade0 mip1 (3D)
            {10, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // cascade0 mip2 (3D)
            {11, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // cascade0 mip3 (3D)
        };
        // Mark SDF textures as 3D view dimension
        bindings[2].is3D = true;
        bindings[3].is3D = true;
        bindings[4].is3D = true;
        bindings[9].is3D = true;
        bindings[10].is3D = true;
        bindings[11].is3D = true;
        indirect_trace_set_layout_ = device_->CreateDescriptorSetLayout({12, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // depth_atlas  [[texture(0)]]
            {1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // normal_atlas [[texture(1)]]
            {2, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf0         [[texture(2)]]
            {3, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf1         [[texture(3)]]
            {4, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // sdf2         [[texture(4)]]
            {5, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // cascade0 mip1 [[texture(5)]] (T3.2)
            {6, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // cascade0 mip2 [[texture(6)]] (T3.2)
            {7, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},   // cascade0 mip3 [[texture(7)]] (T3.2)
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global_cb    [[buffer(0)]]
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params       [[buffer(1)]]
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards        [[buffer(2)]]
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray_hits     [[buffer(3)]]
        };
        DescriptorSetLayoutDesc layoutDesc{12, bindings};
        indirect_trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- IndirectResolve: 4 textures + 1 UBO + 1 UBO + 4 SSBO ---
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
            {9, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev indirect_out (T3.3 EMA)
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // tile validity bitmask (B)
            {11, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // depth_atlas (C: capture validation)
            {12, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // normal_atlas (C: orientation)
            {13, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // tile_grid (A)
            {14, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // tile_aux (A)
            {15, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // prev tile_grid (A EMA)
        };
        indirect_resolve_set_layout_ = device_->CreateDescriptorSetLayout({16, bindings});
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
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // tile validity [[buffer(5)]] (B)
            {6, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev indirect_out [[texture(6)]] (T3.3 EMA)
            {7, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth_atlas   [[texture(7)]] (C)
            {8, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas  [[texture(8)]] (C)
            {9, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // tile_grid     [[texture(9)]] (A)
            {10, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // tile_aux      [[texture(10)]] (A)
            {11, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // prev tile_grid [[texture(11)]] (A EMA)
        };
        DescriptorSetLayoutDesc layoutDesc{16, bindings};
        indirect_resolve_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- IndirectReconstruct (A): tile-grid → validated continuous full atlas ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // albedo (ro)
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth_atlas
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // tile_grid
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // tile_aux
            {6, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // indirect_out
        };
        indirect_reconstruct_set_layout_ = device_->CreateDescriptorSetLayout({7, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params [[buffer(0)]]
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // albedo [[texture(0)]]
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // tile_grid [[texture(1)]]
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // tile_aux [[texture(2)]]
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // indirect_out [[texture(3)]]
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth_atlas [[texture(4)]]
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas [[texture(5)]]
        };
        indirect_reconstruct_set_layout_ = device_->CreateDescriptorSetLayout({7, bindings});
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
    //     write images + 1 UBO + 1 cards SSBO + 2 card-grid SSBOs (T1.3)
    //     + 1 nearest-depth scratch SSBO (A7) ---
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
            {10, DescriptorType::StorageBuffer,    1, ShaderStage::Compute, nullptr},  // A7 capture_depth_min
            // T4.4 v1: emissive 闭环（场景级源，PRIMAL_SC_EMISSIVE 上传）。
            {11, DescriptorType::StorageImage,     1, ShaderStage::Compute, nullptr},  // emissive_atlas
            {12, DescriptorType::UniformBuffer,    1, ShaderStage::Compute, nullptr},  // CaptureEmissive
            {13, DescriptorType::StorageBuffer,    1, ShaderStage::Compute, nullptr},  // instance emissive 表
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[7].readonly = true;
        b[8].readonly = true;
        b[9].readonly = true;
        capture_gather_set_layout_ = device_->CreateDescriptorSetLayout({14, bindings});
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
            // G2（2026-09-16）: PCSS light-space 深度 ×4（与表面直射同源）
            {9,  DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // shadow_depth cascade 0
            {10, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // shadow_depth cascade 1
            {11, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // shadow_depth cascade 2
            {12, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // shadow_depth cascade 3
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[0].is3D = true;
        b[1].is3D = true;
        b[2].is3D = true;
        b[3].readonly = true;
        b[4].readonly = true;
        b[6].readonly = true;
        b[7].readonly = true;
        shadow_set_layout_ = device_->CreateDescriptorSetLayout({13, bindings});
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
            // G2（2026-09-16）: PCSS 深度 ×4 [[texture(5..8)]]
            {5, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // shadow_depth cascade 0
            {6, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // shadow_depth cascade 1
            {7, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // shadow_depth cascade 2
            {8, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr}, // shadow_depth cascade 3
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[0].is3D = true;
        b[1].is3D = true;
        b[2].is3D = true;
        b[3].readonly = true;
        b[4].readonly = true;
        b[6].readonly = true;
        b[7].readonly = true;
        shadow_set_layout_ = device_->CreateDescriptorSetLayout({13, bindings});
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
    // PRIMAL_SC_GATHER_RADIUS=2 selects the LightEval radius-2 (27-point)
    // probe-gather variant (multi-bounce cascade footprint — see
    // SurfaceCacheLightEval.comp). PRIMAL_SC_GATHER_PI=1 selects the physical
    // irradiance variant (×π, restores E=π·L units the legacy path ships
    // deliberately π-deficient for loop headroom). Default keeps the legacy
    // 8-point spv.
    const char* lightEvalName = "SurfaceCacheLightEval";
    if (const char* e = std::getenv("PRIMAL_SC_GATHER_RADIUS")) {
        if (std::atoi(e) >= 2) lightEvalName = "SurfaceCacheLightEval_r2";
    }
    if (const char* e = std::getenv("PRIMAL_SC_GATHER_PI")) {
        if (std::atoi(e) >= 1) {
            lightEvalName = std::getenv("PRIMAL_SC_GATHER_RADIUS") && std::atoi(std::getenv("PRIMAL_SC_GATHER_RADIUS")) >= 2
                ? "SurfaceCacheLightEval_pi_r2"
                : "SurfaceCacheLightEval_pi";
        }
    }
    if (const char* e = std::getenv("PRIMAL_SC_GATHER_RADIUS")) {
        if (std::atoi(e) >= 2) lightEvalName = "SurfaceCacheLightEval_r2";
    }
    auto lightEvalShader    = CompileComputeShader(lightEvalName,
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
    // IndirectReconstruct (A): (8,8,1) over the full atlas — tile-center
    // continuous reconstruction. Built on both backends (Metal kernel twin).
    {
        auto reconstructShader = CompileComputeShader("SurfaceCacheIndirectReconstruct",
            isVk ? "surface_cache_indirect_reconstruct" : "surfaceCacheIndirectReconstruct");
        if (reconstructShader != handles::INVALID_SHADER) {
            {
                PipelineLayoutDesc plDesc;
                plDesc.setLayoutCount = 1;
                plDesc.setLayouts = &indirect_reconstruct_set_layout_;
                indirect_reconstruct_layout_ = device_->CreatePipelineLayout(plDesc);
            }
            ComputePipelineDesc pipeDesc{};
            pipeDesc.computeShader = reconstructShader;
            pipeDesc.layout = indirect_reconstruct_layout_;
            pipeDesc.threadGroupSize = {8, 8, 1};
            indirect_reconstruct_pipeline_ = device_->CreateComputePipeline(pipeDesc);
        }
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
        // A7: write/clear phases — separate SPIR-V files (Vulkan loads one
        // entry point per .comp.spv), same source SurfaceCacheCapture.comp.
        ShaderHandle captureWriteShader = CompileComputeShader("SurfaceCacheCaptureWrite", "surface_cache_capture_write");
        if (captureWriteShader != handles::INVALID_SHADER) {
            ComputePipelineDesc writeDesc = pipeDesc;
            writeDesc.computeShader = captureWriteShader;
            capture_write_pipeline_ = device_->CreateComputePipeline(writeDesc);
        }
        ShaderHandle captureClearShader = CompileComputeShader("SurfaceCacheCaptureClear", "surface_cache_capture_clear");
        if (captureClearShader != handles::INVALID_SHADER) {
            ComputePipelineDesc clearDesc = pipeDesc;
            clearDesc.computeShader = captureClearShader;
            capture_clear_pipeline_ = device_->CreateComputePipeline(clearDesc);
        }
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
        {
            DescriptorSetDesc dsDesc{indirect_reconstruct_set_layout_};
            indirect_reconstruct_set_[i] = device_->CreateDescriptorSet(dsDesc);
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

    // [WRC 计划 Stream D1]（PRIMAL_SC_PRIORITY=1）：卡片重要性打分 + Top-K
    // 替换轮转游标。静态局部定义（文件内 AddPass 侧同读）。
    static const bool kScPriorityCapture = [] {
        const char* e = std::getenv("PRIMAL_SC_PRIORITY");
        return e && e[0] == '1';
    }();

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
            // 单卡片正交捕获（两条调度路径共享；faces = face-slot，帧内唯一）。
            auto captureOne = [&](const SurfaceCacheCard& card, u32 faceSlot) -> bool {
                const uint axis = card.axis_direction & 0xFFu;
                const uint dir  = (card.axis_direction >> 8u) & 0xFFu;

                math::v3 fwd, up;
                float hu = 0.0f, hv = 0.0f, halfAxis = 0.0f;
                // A3: worldToCardUV reads u from a fixed card-local axis
                // (axis0:+y, axis1:+x, axis2:+x) while LookAt's right =
                // cross(fwd, up) flips with dir. For the three combos where
                // right lands on −local_u, mirror the ortho horizontally so
                // the captured atlas matches the lookup convention. Capture shaders
                // orient two-sided normals toward the card eye, independently
                // of the mirrored raster winding (culling is disabled).
                bool mirror_u = false;
                if (axis == 0u) {
                    fwd = math::v3{dir > 0u ? 1.0f : -1.0f, 0.0f, 0.0f};
                    up  = math::v3{0.0f, 0.0f, -1.0f};
                    hu = card.extent.y; hv = card.extent.z; halfAxis = card.extent.x;
                    mirror_u = (dir == 0u);
                } else if (axis == 1u) {
                    fwd = math::v3{0.0f, dir > 0u ? 1.0f : -1.0f, 0.0f};
                    up  = math::v3{0.0f, 0.0f, -1.0f};
                    hu = card.extent.x; hv = card.extent.z; halfAxis = card.extent.y;
                    mirror_u = (dir > 0u);
                } else {
                    fwd = math::v3{0.0f, 0.0f, dir > 0u ? 1.0f : -1.0f};
                    up  = math::v3{0.0f, -1.0f, 0.0f};
                    hu = card.extent.x; hv = card.extent.y; halfAxis = card.extent.z;
                    mirror_u = (dir == 0u);
                }
                if (hu <= 0.0f || hv <= 0.0f || halfAxis <= 0.0f) return false;

                const math::v3 center{card.center.x, card.center.y, card.center.z};
                const math::v3 at = center + fwd;
                const math::m4x4 view = rhimath::CreateLookAtMatrix(center, at, up);
                const math::m4x4 proj = rhimath::CreateOrthographicMatrix(
                    mirror_u ? hu : -hu, mirror_u ? -hu : hu, -hv, hv,
                    0.05f, 2.0f * halfAxis + 0.5f);

                return gpuDraw.ExecuteCardCapturePass(
                    cmd, scene_snapshot, view, proj,
                    math::v4{center.x, center.y, center.z, 1.0f},
                    math::v4{fwd.x, fwd.y, fwd.z, 0.0f},
                    0.05f, 2.0f * halfAxis + 0.5f,
                    faceSlot,       // face_slot — unique within this frame
                    current_frame_index,
                    card.atlas_offset_x, card.atlas_offset_y, card.resolution,
                    albedo_atlas_, normal_atlas_, depth_atlas_, card_view_depth_);
            };

            // [WRC 计划 Stream D1]（PRIMAL_SC_PRIORITY=1）：Top-K 替换轮转
            // 游标——卡片按重要性打分降序取 batch（无相机数据，打分项 =
            // 面积代理 × 新鲜度）；关闭时保留轮转游标逐位不变。
            if (kScPriorityCapture) {
                if (card_capture_last_frame_.size() < total) {
                    card_capture_last_frame_.resize(total, 0u);
                }
                static thread_local std::vector<std::pair<float, u32>> capRanked;
                capRanked.clear();
                capRanked.reserve(total);
                for (u32 k = 0; k < total; ++k) {
                    const auto& c = cards[k];
                    if (c.resolution == 0) continue;
                    const float ex = c.extent.x, ey = c.extent.y, ez = c.extent.z;
                    const uint capAxis = c.axis_direction & 0xFFu;
                    const float hu2 = (capAxis == 0u) ? ey : ex;
                    const float hv2 = (capAxis == 0u) ? ez : (capAxis == 1u ? ez : ey);
                    const u32 age = current_frame_index
                        - std::min(card_capture_last_frame_[k], current_frame_index);
                    const float fresh = std::clamp(float(age) / 30.0f, 0.25f, 1.0f);
                    capRanked.emplace_back(4.0f * hu2 * hv2 * fresh, k);
                }
                std::partial_sort(capRanked.begin(),
                                  capRanked.begin() + std::min<u32>((u32)capRanked.size(), batch),
                                  capRanked.end(),
                                  [](const std::pair<float, u32>& a, const std::pair<float, u32>& b) {
                                      return a.first > b.first;
                                  });
                for (u32 k = 0; k < capRanked.size() && faces < batch; ++k) {
                    const u32 ci = capRanked[k].second;
                    if (captureOne(cards[ci], faces)) {
                        card_capture_last_frame_[ci] = current_frame_index;
                        ++faces;
                    }
                }
                if (faces > 0) card_view_seeded_ = true;
            } else {
                for (u32 k = 0; k < total && faces < batch; ++k) {
                    const u32 ci = (card_view_cursor_ + k) % total;
                    const auto& card = cards[ci];
                    if (card.resolution == 0) continue;
                    if (captureOne(card, faces)) ++faces;
                }

                card_view_cursor_ = (card_view_cursor_ + faces) % total;
                if (card_view_cursor_ == 0 && faces > 0) card_view_seeded_ = true;
            }

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

// T3.3: SC Radiosity bring-up switch (SC_INDIRECT=1) — read once.
// 2026-09-10 十三: tile radiosity promoted to DEFAULT-ON on Vulkan (the
// trace's four stub defects are fixed and W sweep measured monotonic gain;
// W=2.0 → +38% on the darkest wall strips). SC_INDIRECT=0 forces it off.
// 2026-09-11: promoted to DEFAULT-ON on Metal too — its trace/resolve twins
// received the same stub fixes (meters-read march, real hitThreshold, 32
// steps, self-hit origin offset, far-hit atlas resolution, sky tier parity,
// refresh-stride struct fields) and the Metal descriptor branch now binds the
// offline SDF for the tile march.
// SC_INDIRECT env: -1 = unset → default-on on every platform;
// 0/1 force either way.
static const int kScIndirectEnv = [] {
    const char* e = std::getenv("SC_INDIRECT");
    return e ? std::atoi(e) : -1;
}();
// Single source of truth for "does the tile solver run this frame" — BOTH the
// LightEval radiosity_gate.x fill and the trace/resolve dispatch condition
// must read it (the 09-10 十六 "warm entry light that fades" bug was exactly
// these two sites drifting apart).
static bool scIndirectEnabled(rhi::RHIDeviceBase* device) {
    if (kScIndirectEnv >= 0) return kScIndirectEnv != 0;
    (void)device;
    return true;
}
// C3.5 slice 3: warm-up window (frames) — probe feed cut + tile solver held
// back so the atlas converges on direct light deterministically before the
// feedback loops start (multi-stable branch selection happens against the
// transient otherwise). Promoted default 90 (stabilization measured:
// L_arcade runs 2.4/3.1 vs 1.3-3.1 with dark-branch draws).
// Phase 1a: knob moved to GIWarmup.h (single source shared with LumenDDGIPass).
static const u32 scWarmupFrames() { return lumen::GIWarmupFrames(); }
// WRC 计划 Stream B1/B2（能量单向化）：探针馈送改暖启动引导 + tile radiosity
// 单轨。单一开关定义在 GIWarmup.h（GIProbeFeedBootstrapEnabled，DDGI 探针
// 冻结解除同读它）；此处只在每帧 params 填充与 dispatch 条件处消费。
static const bool kProbeFeedBootstrap = lumen::GIProbeFeedBootstrapEnabled();
// WRC 计划 Stream D1（PRIMAL_SC_PRIORITY=1）：卡片重要性打分 + Top-K 替换
// LightEval round-robin 与 card-view 轮转游标。默认 0 = 现有调度逐位不变。
static const bool kScPriority = [] {
    const char* e = std::getenv("PRIMAL_SC_PRIORITY");
    return e && e[0] == '1';
}();
// 打分公式（设计文档 Stream D1）：屏幕占比 × 1/距离² × 新鲜度。
//   coverage : 卡片正面面积 4·ex·ey（extent 为半轴）——屏幕占比的视距无关
//              代理，配合 1/d² 即立体角权重；
//   fresh    : clamp(age / 30, 0.25, 1)——30 帧未服务升满；
//   blend    : mix(1.0, fresh, importance_weight)——复活 importance_weight；
//   距离门   : d > update_distance → score 0——复活 update_distance。
static float ScCardPriority(const SurfaceCacheCard& card, const math::v3& camPos,
                            float importanceWeight, float updateDistance,
                            u32 currentFrame, const u32* lastFrame) {
    const float ex = card.extent.x, ey = card.extent.y, ez = card.extent.z;
    const uint axis = card.axis_direction & 0xFFu;
    const float hu = (axis == 0u) ? ey : ex;
    const float hv = (axis == 0u) ? ez : (axis == 1u ? ez : ey);
    const float coverage = 4.0f * hu * hv;
    const math::v3 dvec{ card.center.x - camPos.x, card.center.y - camPos.y, card.center.z - camPos.z };
    const float dist = std::max(std::sqrt(dvec.x * dvec.x + dvec.y * dvec.y + dvec.z * dvec.z), 1.0f);
    if (updateDistance > 0.0f && dist > updateDistance) return 0.0f;
    const u32 age = lastFrame ? (currentFrame - std::min(*lastFrame, currentFrame)) : 30u;
    const float fresh = std::clamp(float(age) / 30.0f, 0.25f, 1.0f);
    const float w = std::clamp(importanceWeight, 0.0f, 1.0f);
    return (coverage / (dist * dist)) * ((1.0f - w) + w * fresh);
}

SurfaceCacheOutput SurfaceCachePass::AddPass(
    RenderGraph& graph,
    RGResourceHandle prev_frame_color,
    ResourceHandle light_data_buffer,
    const SurfaceCacheFrameData& frame_data,
    u32 current_frame_index)
{
    SurfaceCacheOutput output{};

    // Phase 1d: 槽位单一事实源 DDGISlotMap.h（outIdx = writeIndex）。
    const u32 outIdx = DDGISlotsForFrame(current_frame_index).writeIndex;

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
        [prev_frame_color, lightingHandle, frame_data](
            SurfaceCacheRGData& data, RenderGraphBuilder& builder) {
            // Read previous frame color
            builder.Read(prev_frame_color, ResourceState::ShaderResource);

            data.prev_frame_color = prev_frame_color;

            // G2（2026-09-16）: PCSS light-space 深度——shadow dispatch 每帧
            // 采样（与表面直射同源同帧）。无效句柄跳过（cascade count 随
            // 之为 0，shader 走 SDF march 回退）。
            for (u32 c = 0; c < 4; ++c) {
                if (frame_data.shadow_depth[c].IsValid())
                    builder.Read(frame_data.shadow_depth[c], ResourceState::ShaderResource);
            }

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

            const u32 frameIdx = DDGISlotsForFrame(current_frame_index).writeIndex;

            // Guard: pipeline must be valid
            if (light_eval_pipeline_ == handles::INVALID_PIPELINE) return;

            // ================================================================
            // 1. Build CardDispatchInfo from active cards (prefix-sum)
            // ================================================================
            const auto& cards = card_generator_.GetCards();
            u32 card_count = card_generator_.GetCardCount();
            u32 total_texels = 0;

            std::vector<CardDispatchInfo> dispatch_info(card_count);
            // [WRC 计划 Stream D1]: 卡片集重建后新鲜度表跟随扩容（新卡 0 =
            // 最陈旧，首轮即被 Top-K 拾起）。
            if (card_light_last_frame_.size() < cards.size()) {
                card_light_last_frame_.resize(cards.size(), 0u);
            }
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
                // PRIMAL_SC_BUDGET：页预算上限（页/帧）。0 = 全量更新——
                // Vulkan 基准行为（实测 374 卡/帧 vs Metal 预算后 8 卡/帧）。
                // SC atlas 覆盖不足曾是 Metal finalize 的 SC-first 层级退化
                // 为"黑+解析蓝"的根因（0c2e34d9 因此错误地改用 prev-first）。
                // A6: env 与 governor 仲裁——env 是上限，governor 每帧
                // SetPageBudget 的缩放值取 min 生效；不再无条件覆盖 governor。
                static const u32 s_budgetOverride = [] {
                    const char* e = std::getenv("PRIMAL_SC_BUDGET");
                    return e ? (u32)std::atoi(e) : 0xFFFFFFFFu;
                }();
                if (s_budgetOverride != 0xFFFFFFFFu) {
                    static bool s_budgetWarned = false;
                    if (!s_budgetWarned) {
                        s_budgetWarned = true;
                        PRIMAL_LOG_STREAM(Warn, "Render")
                            << "[SurfaceCache] PRIMAL_SC_BUDGET=" << s_budgetOverride
                            << " active as a ceiling; the GI governor scales the "
                            << "per-frame page budget below it (min arbitration)."
                            << std::endl;
                    }
                }
                u32 budgetPages = config_.surface_cache_capture_budget;
                if (s_budgetOverride != 0xFFFFFFFFu) {
                    // env=0 → 全量更新（原语义）；config==0（Low 档）时 env 仍为上限；
                    // 两者皆有限取 min（governor 只能压到 env 之下，不能抬高）。
                    budgetPages = (s_budgetOverride == 0)
                        ? 0u
                        : (budgetPages == 0 || budgetPages > s_budgetOverride)
                            ? s_budgetOverride
                            : budgetPages;
                }
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

                    // [WRC 计划 Stream D1]（PRIMAL_SC_PRIORITY=1）：Top-K 替换
                    // round-robin——卡片按重要性打分降序装入页预算窗口。
                    // 打分项 = 屏幕占比(面积代理) × 1/dist²(相机距离) ×
                    // 新鲜度(30 帧未服务升满)，importance_weight 调新鲜度
                    // 权重，update_distance 外的卡片 score=0。
                    bool topkApplied = false;
                    if (kScPriority && card_light_last_frame_.size() >= source_card_count) {
                        static thread_local std::vector<std::pair<float, u32>> ranked;
                        ranked.clear();
                        ranked.reserve(source_card_count);
                        const math::v3 camPos = frame_data.camera_position;
                        for (u32 k = 0; k < source_card_count; ++k) {
                            const CardDispatchInfo& di = dispatch_info[k];
                            const SurfaceCacheCard& c = cards[di.card_index];
                            const float score = ScCardPriority(
                                c, camPos,
                                config_.surface_cache_importance_weight,
                                config_.surface_cache_update_distance,
                                current_frame_index,
                                &card_light_last_frame_[di.card_index]);
                            ranked.emplace_back(score, k);
                        }
                        std::partial_sort(ranked.begin(),
                                          ranked.begin() + std::min<u32>(source_card_count, 64u),
                                          ranked.end(),
                                          [](const std::pair<float, u32>& a, const std::pair<float, u32>& b) {
                                              return a.first > b.first;
                                          });
                        u32 acc2 = 0, taken2 = 0;
                        std::vector<CardDispatchInfo> selected;
                        for (u32 k = 0; k < source_card_count && taken2 < 64u; ++k) {
                            const CardDispatchInfo& di = dispatch_info[ranked[k].second];
                            const u32 cnt = di.texel_count;
                            if (cnt == 0) continue;
                            if (taken2 > 0 && acc2 + cnt > texel_budget) break;
                            acc2 += cnt;
                            ++taken2;
                            CardDispatchInfo out = di;
                            out.texel_offset = 0;   // rebased below
                            selected.push_back(out);
                            card_light_last_frame_[di.card_index] = current_frame_index;
                        }
                        if (taken2 > 0 && taken2 < source_card_count) {
                            u32 off = 0;
                            for (auto& out : selected) { out.texel_offset = off; off += out.texel_count; }
                            dispatch_info = std::move(selected);
                            card_count = taken2;
                            total_texels = off;
                            // 轮转游标仍推进（下一帧预算窗口起点公平移动）。
                            light_eval_rr_cursor_ = (light_eval_rr_cursor_ + taken2) % source_card_count;
                            topkApplied = true;
                        }
                        // Top-K 全量装下（罕见）→ 落回 legacy 轮转逻辑。
                    }
                    if (!topkApplied) {
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
                    // A4: upload the real per-slot light list when the caller
                    // provided one (type/position/radius per slot) — keeps the
                    // point-light path in LightEval alive. Legacy single
                    // directional slot remains the fallback.
                    const u32 realCount = frame_data.sc_light_count;
                    if (realCount > 0) {
                        for (u32 i = 0; i < realCount; ++i) {
                            lights[i].position  = frame_data.sc_lights[i * 3 + 0];
                            lights[i].color     = frame_data.sc_lights[i * 3 + 1];
                            lights[i].direction = frame_data.sc_lights[i * 3 + 2];
                        }
                    } else {
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
                    params.light_count = frame_data.sc_light_count > 0
                        ? frame_data.sc_light_count
                        : (frame_data.light_count > 0 ? frame_data.light_count : 1u);
                    params.opensky_v2 = 0;   // 原零化 _pad 槽；Phase 4a 起为 openSky V2 门（env 覆盖在下方）
                    // Probe-fed indirect grid (Lumen hierarchy); valid flag 0
                    // keeps the shader on the legacy self-feedback path.
                    params.ddgi_origin_spacing[0] = frame_data.ddgi_origin.x;
                    params.ddgi_origin_spacing[1] = frame_data.ddgi_origin.y;
                    params.ddgi_origin_spacing[2] = frame_data.ddgi_origin.z;
                    params.ddgi_origin_spacing[3] = frame_data.ddgi_spacing;
                    params.ddgi_counts_valid[0] = frame_data.ddgi_counts[0];
                    params.ddgi_counts_valid[1] = frame_data.ddgi_counts[1];
                    params.ddgi_counts_valid[2] = frame_data.ddgi_counts[2];
                    // C1-H3: the card_dispatch stand-in binding (INVALID feed)
                    // must never be read — force the flag off at the pass
                    // boundary regardless of what the caller set.
                    // SC_PROBE_FEED_OFF=1（C 系列归因旋钮）：切断 probe→SC
                    // 馈送。gatherProbeIrradiance 是无可见性 8-probe 三线性，
                    // 封闭区域卡片隔墙采到亮探针 = 室内平填充的头号泄漏入口；
                    // 置 0 回落 legacy 自反馈路径做干预式 A/B。
                    // C3.5 slice 3: during the warm-up window the probe feed
                    // is cut so LightEval's texels converge on direct+capture
                    // alone (deterministic) — matches the DDGI probe freeze
                    // (PRIMAL_SC_WARMUP_FRAMES).
                    // WRC 计划 Stream B1: bootstrap 模式下馈送从第 0 帧生效，
                    // 由 probe_feed_bootstrap 因子在窗口内线性衰减到 0——
                    // "窗口后常开"的 E1 反馈边改为一次性引导。
                    params.ddgi_counts_valid[3] =
                        (frame_data.ddgi_irradiance != rhi::handles::INVALID_RESOURCE &&
                         !std::getenv("SC_PROBE_FEED_OFF") &&
                         (kProbeFeedBootstrap || current_frame_index >= scWarmupFrames()))
                            ? frame_data.ddgi_valid : 0u;
                    // T3.3: gate the LightEval tile-radiosity read on whether
                    // the indirect trace/resolve actually ran this session —
                    // indirect_out holds uninitialized data otherwise.
                    // Gate.x must follow the DISPATCH condition exactly —
                    // during the warm-up window the solver does not run, and
                    // reading the never-written indirect_out here fed
                    // uninitialized heap memory into the atlas (the "warm
                    // entry light that fades" report).
                    // WRC 计划 Stream B2: bootstrap 模式下 solver 从第 0 帧运行
                    //（dispatch 条件同步放开）；消费端从第 1 帧读——LightEval
                    // 在同一 AddPass 里先于 trace/resolve 执行，第 0 帧读到的
                    // 是未初始化堆内存（同一历史 bug 的边界）。
                    params.radiosity_gate[0] =
                        scIndirectEnabled(device_) &&
                        (kProbeFeedBootstrap
                            ? current_frame_index >= 1u
                            : current_frame_index >= scWarmupFrames())
                            ? 1u : 0u;
                    // C3.5 slice 2: tile-radiosity tile size for the LightEval
                    // consumption read (0 = shader-side legacy 16).
                    params.radiosity_gate[2] = [] {
                        const char* e = std::getenv("PRIMAL_SC_RADIOSITY_TILE");
                        if (!e) return 8u;   // LightEval consumption grid follows
                        const u32 v = (u32)std::atoi(e);
                        return (v >= 4u && v <= 128u) ? v : 8u;
                    }();
                    // T3.3 runtime weight knob: radiosity_gate.y packs the
                    // tile-radiosity consumption weight as raw float bits
                    // (0 = shader-side legacy 0.25). PRIMAL_SC_RADIOSITY_W.
                    params.radiosity_gate[1] = [] {
                        // Promoted default 2.0 (W sweep: monotonic gain to
                        // +38% on the darkest strips, stable at frame 800).
                        const char* e = std::getenv("PRIMAL_SC_RADIOSITY_W");
                        const float v = e ? (float)std::atof(e) : 2.0f;
                        if (v <= 0.0f || v > 4.0f) return 0u;
                        u32 bits; std::memcpy(&bits, &v, 4);
                        return bits;
                    }();
                    // GI 根治 Phase 3.2: radiosity_gate.w = openSky 分类开关
                    // （数据源 ddgi_opensky 无效时关闭；PRIMAL_GI_OPENSKY=0 回滚）。
                    {
                        bool openskyOn = frame_data.ddgi_opensky != handles::INVALID_RESOURCE;
                        if (const char* e = std::getenv("PRIMAL_GI_OPENSKY")) {
                            if (e[0] == '0') openskyOn = false;
                        }
                        params.radiosity_gate[3] = openskyOn ? 1u : 0u;
                        // 能量所有权 Phase 4a: openSky V2（E_geo/E_sky 拆分），
                        // PRIMAL_GI_OPENSKY_V2=0 回退旧整探针分类。
                        params.opensky_v2 = [] {
                            const char* e = std::getenv("PRIMAL_GI_OPENSKY_V2");
                            return (e && e[0] == '0') ? 0u : 1u;
                        }();
                    }
                    // WRC 计划 Stream B1/B2（能量单向化）：引导因子 + 单轨标志。
                    // 旧语义（bootstrap 关）factor=1、solo=0，shader 端逐位不变。
                    params.probe_feed_bootstrap =
                        lumen::GIProbeBootstrapFactor(current_frame_index);
                    params.radiosity_solo = kProbeFeedBootstrap ? 1u : 0u;
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
                ResourceBarrier atlasBarriers[6]{};
                atlasBarriers[0].resource = albedo_atlas_;
                atlasBarriers[1].resource = normal_atlas_;
                atlasBarriers[2].resource = depth_atlas_;
                atlasBarriers[3].resource = emissive_atlas_;
                // P0-B: tile_aux 同样在 pass 入口收敛到 GENERAL——它是
                // resolve 的 storage 写目标（rp2[14]），首帧 tracked=UNDEF
                // 时写前必须有转换；后续帧由 recon 回置保持 GENERAL。
                atlasBarriers[4].resource = indirect_tile_aux_;
                atlasBarriers[5].resource = indirect_tile_tex_[DDGISlotsForFrame(current_frame_index).writeIndex];
                for (auto& b : atlasBarriers) {
                    b.beforeState = ResourceState::UnorderedAccess;
                    b.afterState  = ResourceState::UnorderedAccess;
                    b.subresource = 0xFFFFFFFF;
                }
                cmd->InsertBarrier(atlasBarriers, 6);
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
                // GI 能量所有权修复 Phase 5b: Vulkan 上 GBuffer 已进 RG
                // （NaniteSceneRender Write 声明 → producer identity），RG
                // 插入正式 Write→Read 转换，此 no-op barrier 不再需要。
                // Metal 仍保留（encoder 边界语义另案，parity 矩阵已记）。
                const bool gbufferRG = device_->GetPlatform() == rhi::RHIPlatform::Vulkan &&
                    [](const char* e) { return !(e && e[0] == '0'); }(std::getenv("PRIMAL_GBUFFER_RG"));
                if (!gbufferRG) {
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
                        u32 atlas_size;
                    };
                    auto* mapped = static_cast<CaptureParamsCB*>(device_->MapBuffer(capture_cb_[frameIdx]));
                    if (mapped) {
                        mapped->invViewProjection = frame_data.inv_view_projection;
                        mapped->card_count    = card_count;
                        mapped->render_width  = frame_data.render_width;
                        mapped->render_height = frame_data.render_height;
                        mapped->atlas_size = atlas_size_;
                        device_->UnmapBuffer(capture_cb_[frameIdx]);
                    }
                }

                // T4.4 v1: capture emissive 源（PRIMAL_SC_EMISSIVE="r,g,b"，默认 0）。
                {
                    static const math::v4 s_sceneEmissive = [] {
                        const char* e = std::getenv("PRIMAL_SC_EMISSIVE");
                        if (!e) return math::v4{0.0f, 0.0f, 0.0f, 0.0f};
                        float r = 0, g = 0, b = 0;
# ifdef _MSC_VER
                        if (sscanf_s(e, "%f,%f,%f", &r, &g, &b) != 3) return math::v4{0.0f, 0.0f, 0.0f, 0.0f};
# else
                        if (std::sscanf(e, "%f,%f,%f", &r, &g, &b) != 3) return math::v4{0.0f, 0.0f, 0.0f, 0.0f};
# endif
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[SC] capture emissive = (" << r << "," << g << "," << b << ")" << std::endl;
                        return math::v4{r, g, b, 1.0f};
                    }();
                    auto* em = static_cast<math::v4*>(device_->MapBuffer(capture_emissive_cb_[frameIdx]));
                    if (em) { *em = s_sceneEmissive; device_->UnmapBuffer(capture_emissive_cb_[frameIdx]); }
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
                    {10, DescriptorType::StorageBuffer,    capture_min_depth_buffer_},
                    // T4.4 v1: emissive 写入 + 场景级源 + per-instance 表
                    {11, DescriptorType::StorageImage,     emissive_atlas_},
                    {12, DescriptorType::UniformBuffer,    capture_emissive_cb_[frameIdx]},
                    {13, DescriptorType::StorageBuffer,    instance_emissive_buffer_},
                };
                UpdateDescriptorSet(device_, capture_gather_set_[frameIdx], captureParams, 14);

                cmd->BindComputePipeline(capture_gather_pipeline_);
                const DescriptorSetHandle capSets[] = { capture_gather_set_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, capture_gather_layout_, 0, 1, capSets, 0, nullptr);

                // A7 three-dispatch nearest-wins capture:
                //   clear → (barrier) → atomicMin phase → (barrier) → write phase.
                const u32 atlasGroups = (atlas_size_ + 7) / 8;
                if (capture_clear_pipeline_ != handles::INVALID_PIPELINE) {
                    cmd->BindComputePipeline(capture_clear_pipeline_);
                    cmd->Dispatch(atlasGroups, atlasGroups, 1);
                    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                       AccessFlag::ShaderWrite, AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
                    cmd->BindComputePipeline(capture_gather_pipeline_);
                }

                cmd->Dispatch((frame_data.render_width + 7) / 8,
                              (frame_data.render_height + 7) / 8, 1);

                if (capture_write_pipeline_ != handles::INVALID_PIPELINE) {
                    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                       AccessFlag::ShaderWrite, AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
                    cmd->BindComputePipeline(capture_write_pipeline_);
                    cmd->Dispatch((frame_data.render_width + 7) / 8,
                                  (frame_data.render_height + 7) / 8, 1);
                }

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

                        // ShadowParams (mirror of the shader UBO)。
                        // G2（2026-09-16）: 尾扩 shadow 矩阵（列主序 4 级联）
                        // + meta（x=PCSS 级联数，>0.5 启用 shadow-map 优先
                        // 可见性；y=texel 法线偏移缩放）。
                        struct ShadowParamsGPU {
                            math::v4 sdf_origins[3];
                            math::v4 sdf_extents[3];
                            math::v4 sdf_res_count;
                            math::v4 light_dir_toward;
                            math::v4 shadow_params;   // x=maxDist, y=bias, z=atlasSize
                            math::v4 counts;          // x=total_texels, y=card_count
                            math::m4x4 shadow_matrices[4];  // lightProj*lightView（列主序）
                            math::v4 shadow_meta;     // x=cascade_count, y=normal_offset_scale
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
                            // G2: PCSS 矩阵/级联数（frame_data 无效矩阵保持
                            // 全零 → meta.x=0 → shader 走 SDF march 回退）
                            u32 sc = 0;
                            for (u32 c = 0; c < 4 && c < frame_data.shadow_cascade_count; ++c) {
                                if (!frame_data.shadow_depth[c].IsValid()) break;
                                sp->shadow_matrices[c] = frame_data.shadow_matrices[c];
                                ++sc;
                            }
                            sp->shadow_meta = {(float)sc, 0.75f, 0.0f, 0.0f};
                            device_->UnmapBuffer(shadow_cb_[frameIdx]);
                        }

                        // G2: 解析 shadow depth RG → 物理（无效槽位回退
                        // depth_atlas 占位——cascade_count 已反映有效性，
                        // shader 不会采样无效槽位）
                        ResourceHandle depthTex[4];
                        for (u32 c = 0; c < 4; ++c) {
                            // 占位用 SRV-only dummy（depth_atlas 是 readonly
                            // storage → GENERAL 布局，不能同时作 SampledImage）
                            depthTex[c] = indirect_dummy_tex_;
                            if (c < frame_data.shadow_cascade_count &&
                                frame_data.shadow_depth[c].IsValid()) {
                                auto* res = context.graph->GetResource(frame_data.shadow_depth[c]);
                                if (res) depthTex[c] = res->GetPhysicalHandle();
                            }
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
                            {shIsVk ? 9u : 4u,  DescriptorType::SampledImage, depthTex[0]},
                            {shIsVk ? 10u : 5u, DescriptorType::SampledImage, depthTex[1]},
                            {shIsVk ? 11u : 6u, DescriptorType::SampledImage, depthTex[2]},
                            {shIsVk ? 12u : 7u, DescriptorType::SampledImage, depthTex[3]},
                        };
                        UpdateDescriptorSet(device_, shadow_set_[frameIdx], shadowParams, 13);

                        cmd->BindComputePipeline(shadow_pipeline_);
                        const DescriptorSetHandle shSets[] = { shadow_set_[frameIdx] };
                        cmd->BindDescriptorSets(PipelineBindPoint::Compute, shadow_layout_, 0, 1, shSets, 0, nullptr);

                        u32 shGroups = (total_texels + 255) / 256;
                        cmd->Dispatch(shGroups, 1, 1);

                        // visibility write → LightEval read
                        cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                           AccessFlag::ShaderWrite, AccessFlag::ShaderRead);

                        // T3.3 诊断：可见性缓冲统计（直射项归零排查）
                        if (std::getenv("SC_DUMP_CARDS")) {
                            static u32 visDumpFrame = 0;
                            if (current_frame_index % 60u == 30u && visDumpFrame != current_frame_index) {
                                visDumpFrame = current_frame_index;
                                DebugShadowVisibilityStats();
                            }
                        }
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
                // the multi-bounce feedback source. Phase 1d: (N+1)%3 与
                // DDGISlotMap 恒等（N-2 ≡ N+1 mod 3，+1 写法避免 unsigned
                // 下溢），槽位语义以 DDGISlotMap.h 为准。
                const u32 histIdx = DDGISlotsForFrame(current_frame_index + 1u).currentIndex;
                DescriptorData evalParams[15];
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
                        {12, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]},   // T3.3 tile radiosity
                        // C 系列：探针深度（Chebyshev gather 可见性）；无效时绑
                        // stand-in（CB flag 关 → shader 不读）。
                        {13, DescriptorType::StorageBuffer,
                         frame_data.ddgi_depth != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_depth
                             : card_dispatch_buffer_},
                        // GI 根治 Phase 3.2: openSky 统计（无效时绑 stand-in，
                        // radiosity_gate.z 关 → shader 不读）。
                        {14, DescriptorType::StorageBuffer,
                         frame_data.ddgi_opensky != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_opensky
                             : card_dispatch_buffer_},
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
                        // T3.3 tile radiosity (Metal texture namespace 6)
                        {6, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]},
                        // C 系列：探针深度（Chebyshev gather 可见性）
                        {7, DescriptorType::StorageBuffer,
                         frame_data.ddgi_depth != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_depth
                             : card_dispatch_buffer_},
                        // GI 根治 Phase 3.2: openSky 统计（Metal buffer(8)）
                        {8, DescriptorType::StorageBuffer,
                         frame_data.ddgi_opensky != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_opensky
                             : card_dispatch_buffer_},
                    };
                    memcpy(evalParams, p, sizeof(p));
                }
                // C 系列诊断：深度缓冲真值（量级/单位校验）
                if (std::getenv("SC_DEPTH_STATS")) {
                    static u32 dsFrame = 0;
                    if (frame_data.ddgi_depth != handles::INVALID_RESOURCE &&
                        current_frame_index % 120u == 60u && dsFrame != current_frame_index) {
                        dsFrame = current_frame_index;
                        const u64 depthFloats = u64(16384) * 128;
                        if (auto* d = static_cast<const float*>(device_->MapBuffer(frame_data.ddgi_depth, 0, depthFloats * sizeof(float)))) {
                            double s0 = 0, s1 = 0;
                            for (int i = 0; i < 64; ++i) s0 += d[i];
                            for (int i = 0; i < 128; ++i) s1 += d[i];
                            u32 camProbe = 7947; // camera-cell (11,8,15)
                            double cMean = 0;
                            for (int i = 0; i < 64; ++i) cMean += d[camProbe * 128 + i];
                            PRIMAL_LOG_STREAM(Error, "Render") << "[SC_DEPTH_STATS] probe0 mean_avg="
                                      << s0 / 64.0 << " all128_avg=" << s1 / 128.0
                                      << " camcell7947 mean_avg=" << cMean / 64.0
                                      << " cam8=(" << d[camProbe * 128 + 0] << "," << d[camProbe * 128 + 1]
                                      << "," << d[camProbe * 128 + 2] << "," << d[camProbe * 128 + 3]
                                      << "," << d[camProbe * 128 + 4] << "," << d[camProbe * 128 + 5]
                                      << "," << d[camProbe * 128 + 6] << "," << d[camProbe * 128 + 7] << ")" << std::endl;
                            device_->UnmapBuffer(frame_data.ddgi_depth);
                        }
                    }
                }

                UpdateDescriptorSet(device_, light_eval_set_[frameIdx], evalParams, 15);

                // Explicit GENERAL transition for the lighting atlases — do not
                // rely on the render graph's Write declaration having put them
                // in a storage-compatible layout before this dispatch. Both the
                // written slot (outIdx) and the multi-bounce feedback slot
                // (histIdx) are bound as storage images; histIdx is not RG-
                // declared, and on its first frames it is still UNDEFINED.
                // The Vulkan backend resolves oldLayout from the tracked layout,
                // so the first such barrier correctly performs UNDEFINED→UAV.
                {
                    // P0-B（01197 定位）: indirect_out[outIdx] 也在 LightEval
                    // 的 storage 写列表（binding 12）——上一帧它作为 resolve 的
                    // EMA history 被 sampled（tracked=SRO），写前必须转回
                    // GENERAL；否则 storage 写在 SRO 上发生（UB，validation 钉
                    // real=GENERAL），后续 resolve 前置 UA barrier 触发 01197。
                    ResourceBarrier lb[3]{};
                    lb[0].resource = lighting_atlas_[outIdx];
                    lb[1].resource = lighting_atlas_[histIdx];
                    lb[2].resource = indirect_out_tex_[outIdx];
                    for (auto& b : lb) {
                        b.beforeState = ResourceState::UnorderedAccess;
                        b.afterState  = ResourceState::UnorderedAccess;
                        b.subresource = 0xFFFFFFFF;
                    }
                    cmd->InsertBarrier(lb, 3);
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

                // ================================================================
                // T3.3 (D10): indirect trace + resolve dispatch wiring (the
                // pipelines existed since M4.4 but were never dispatched).
                // OPT-IN (SC_INDIRECT=1): the 128×128 tile dispatches ×2 cost
                // ~10× frame budget on Sponza; the LightEval tile-probe
                // consumer lands in the next slice, until then this stays a
                // bring-up path.
                const bool scIndirectOn = scIndirectEnabled(device_);
                // WRC 计划 Stream B2: bootstrap 模式下 solver 从第 0 帧运行
                //（LightEval 消费端 gate.x 同步改为 frame>=1，见 params 填充处）。
                if (scIndirectOn &&
                    (kProbeFeedBootstrap || current_frame_index >= scWarmupFrames()))
                // trace: per-tile cosine ray fan → SDF march → ray_hits.
                // resolve: aggregate ray_hits and expand into full-atlas indirect_out.
                // Consumers (LightEval tile probe) land in the next slice.
                // ================================================================
                {
                    // C3.5 slice 2: runtime tile size + trace stride.
                    // PRIMAL_SC_RADIOSITY_TILE (power-of-two, resolved once —
                    // see SCRadiosityTileSize), PRIMAL_SC_RADIOSITY_STRIDE
                    // (default 1 = every tile every frame; N = each tile traced
                    // every Nth frame, phase rotating with frame_index).
                    const u32 kTile = std::min<u32>(SCRadiosityTileSize(), atlas_size_);
                    const u32 kStride = [] {
                        const char* e = std::getenv("PRIMAL_SC_RADIOSITY_STRIDE");
                        if (!e) return 2u;   // promoted default: half-rate trace
                        const u32 v = (u32)std::atoi(e);
                        return (v >= 1u && v <= 64u) ? v : 1u;
                    }();
                    // A (plan §三.A): continuous reconstruction on by default;
                    // PRIMAL_SC_TILE_RECON=0 keeps the legacy piecewise-constant
                    // expansion for A/B evidence.
                    static const bool kTileRecon = [] {
                        const char* e = std::getenv("PRIMAL_SC_TILE_RECON");
                        return !e || std::atoi(e) != 0;
                    }();
                    const bool reconActive = kTileRecon &&
                        indirect_reconstruct_pipeline_ != handles::INVALID_PIPELINE;
                    const u32 kRays = 32;
                    const u32 kTiles = atlas_size_ / kTile;
                    // G (plan §三.G): ray_hits capacity follows the runtime
                    // tile config — tilesX×tilesY×raysPerProbe×32B
                    // (ProbeRayHit = two vec4s, both backends). The old
                    // totalPages×page²×16B formula was only coincidentally
                    // sufficient at tile=8; tile=4 needed 4× that and
                    // silently wrote out of bounds.
                    constexpr u64 kRayHitStride = 32;   // GLSL ProbeRayHit / MSL ProbeRayHit
                    const u64 kTotalHits = (u64)kTiles * kTiles * kRays;
                    // Overflow guard (64Mi hits × 32B = 2 GiB cap): zero the
                    // dispatch grid for this session instead of under-
                    // allocating — a zero-group dispatch is a no-op on both
                    // backends, and tile_size=0 early-outs the shaders.
                    u32 kTilesEff = kTiles;
                    if (kTotalHits == 0 || kTotalHits > (u64)64 * 1024 * 1024) {
                        static bool gOverflowLogged = false;
                        if (!gOverflowLogged) {
                            gOverflowLogged = true;
                            PRIMAL_LOG_STREAM(Error, "Render")
                                << "[SurfaceCache] ray_hits capacity overflow: tiles="
                                << kTiles << " rays=" << kRays << " hits=" << kTotalHits
                                << " — tile radiosity disabled this session" << std::endl;
                        }
                        kTilesEff = 0;
                    } else {
                    // T5.5: lazy allocation on first opt-in dispatch
                    if (ray_hits_buffer_ == handles::INVALID_RESOURCE ||
                        ray_hits_capacity_ != kTotalHits) {
                        if (ray_hits_buffer_ != handles::INVALID_RESOURCE) {
                            device_->DestroyBuffer(ray_hits_buffer_);
                            ray_hits_buffer_ = handles::INVALID_RESOURCE;
                        }
                        BufferDesc rayHitsDesc{};
                        rayHitsDesc.size = kTotalHits * kRayHitStride;
                        rayHitsDesc.type = BufferType::Structured;
                        rayHitsDesc.usage = GPUMemoryUsage::Dynamic;
                        rayHitsDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                        ray_hits_buffer_ = device_->CreateBuffer(rayHitsDesc);
                        ray_hits_capacity_ = kTotalHits;
                        // Hits start zeroed: with the B validity contract the
                        // contents are never read before a trace writes them,
                        // but a deterministic initial state keeps readback
                        // diagnostics honest.
                        if (ray_hits_buffer_ != handles::INVALID_RESOURCE) {
                            if (void* z = device_->MapBuffer(ray_hits_buffer_, 0, rayHitsDesc.size)) {
                                std::memset(z, 0, rayHitsDesc.size);
                                device_->UnmapBuffer(ray_hits_buffer_);
                            }
                        }
                    }

                    // B (plan §三.B): per-tile validity bitmask. Config
                    // signature = tile grid + card-set identity — a changed
                    // tile size or regenerated card set zeroes every tile's
                    // validity (history must not survive a geometry/config
                    // change it was not captured against).
                    const u64 tileSig = ((u64)kTiles << 32) ^
                                        (u64)card_generator_.GetLookupCount() ^
                                        ((u64)card_generator_.GetCards().size() << 8);
                    const u64 tileWords = ((u64)kTiles * kTiles + 31u) / 32u;
                    // GI 能量所有权修复 Phase 5a: 灯光修改事件强制清零一次
                    // （temporal invalidation——tile EMA 历史不得跨灯光变化
                    // 存活）。清零后恢复常规 sig 门控。
                    const bool forceTileReset = tile_validity_reset_pending_;
                    tile_validity_reset_pending_ = false;
                    if (indirect_tile_valid_buffer_ == handles::INVALID_RESOURCE ||
                        indirect_tile_config_sig_ != tileSig || forceTileReset) {
                        if (indirect_tile_valid_buffer_ != handles::INVALID_RESOURCE) {
                            device_->DestroyBuffer(indirect_tile_valid_buffer_);
                            indirect_tile_valid_buffer_ = handles::INVALID_RESOURCE;
                        }
                        BufferDesc tvDesc{};
                        tvDesc.size = tileWords * sizeof(u32);
                        tvDesc.type = BufferType::Raw;
                        tvDesc.usage = GPUMemoryUsage::Dynamic;
                        tvDesc.memoryUsage = GPUMemoryUsage::Dynamic;
                        indirect_tile_valid_buffer_ = device_->CreateBuffer(tvDesc);
                        indirect_tile_config_sig_ = tileSig;
                        if (indirect_tile_valid_buffer_ != handles::INVALID_RESOURCE) {
                            if (void* z = device_->MapBuffer(indirect_tile_valid_buffer_, 0, tvDesc.size)) {
                                std::memset(z, 0, tvDesc.size);
                                device_->UnmapBuffer(indirect_tile_valid_buffer_);
                            }
                            static u64 lastResetSig = 0;
                            if (lastResetSig != tileSig) {
                                lastResetSig = tileSig;
                                PRIMAL_LOG_STREAM(Info, "Render")
                                    << "[SurfaceCache] tile validity reset (tiles=" << kTiles
                                    << " cards=" << card_generator_.GetCards().size() << ")" << std::endl;
                            }
                        }
                    }
                    }

                    // C3.5 slice 4: offline propagated SDF for the tile march
                    // (thin-arch sky leaks in the coarse runtime GlobalSDF).
                    const bool useOfflineSdf = sc_offline_sdf_texture_ != handles::INVALID_RESOURCE;

                    auto& gsdf = nanite::GlobalSDF::Get();

                    // ---- trace params upload ----
                    if (auto* tp = static_cast<IndirectTraceParamsGPU*>(
                            device_->MapBuffer(indirect_trace_params_cb_[frameIdx]))) {
                        *tp = IndirectTraceParamsGPU{};
                        tp->sc_params.atlas_size = atlas_size_;
                        tp->sc_params.page_size = page_size_;
                        tp->sc_params.max_cards = config_.surface_cache_max_cards;
                        tp->sc_params.lookup_count = card_generator_.GetLookupCount();
                        tp->sc_params.indirect_rays_per_probe = kRays;
                        tp->tile_size = kTilesEff != 0u ? kTile : 0u;
                        tp->rays_per_probe = kRays;
                        tp->frame_index = current_frame_index;
                        tp->near_distance = 0.25f;
                        tp->max_ray_distance = 50.0f;
                        tp->refresh_stride = kStride;
                        tp->refresh_phase = kStride > 1u ? current_frame_index % kStride : 0u;
                        // C3.5 slice 4: prefer the OFFLINE propagated SDF
                        // (384³, signed, real distances — same source the DDGI
                        // trace marches). The runtime GlobalSDF's coarse
                        // cascades leak thin arches as false sky in the tile
                        // traces (measured: occluded soffits glowing). Fall
                        // back to GlobalSDF only when no offline source.
                        u32 cc = std::min<u32>(3u, gsdf.GetConfig().cascade_count);
                        if (useOfflineSdf) {
                            cc = 1u;
                            tp->sdf_origins[0] = math::v4{sc_offline_sdf_origin_.x, sc_offline_sdf_origin_.y,
                                                          sc_offline_sdf_origin_.z, 0.0f};
                            tp->sdf_extents[0] = math::v4{sc_offline_sdf_extent_.x, sc_offline_sdf_extent_.y,
                                                          sc_offline_sdf_extent_.z, 0.0f};
                            tp->sdf_resolutions[0] = math::u32v4{sc_offline_sdf_resolution_, 0, 0, 0};
                            tp->sdf_voxel_sizes[0] = math::v4{
                                sc_offline_sdf_extent_.x / (float)sc_offline_sdf_resolution_, 0.0f, 0.0f, 0.0f};
                        } else {
                        for (u32 c = 0; c < 3; ++c) {
                            if (c < cc) {
                                const auto& cascade = gsdf.GetCascade(c);
                                tp->sdf_origins[c] = math::v4{cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f};
                                tp->sdf_voxel_sizes[c] = math::v4{cascade.voxel_size, 0.0f, 0.0f, 0.0f};
                                tp->sdf_extents[c] = math::v4{cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f};
                                tp->sdf_resolutions[c] = math::u32v4{cascade.resolution, 0, 0, 0};
                            }
                        }
                        }
                        tp->sdf_cascade_count = math::u32v4{cc, 0, 0, 0};
                        device_->UnmapBuffer(indirect_trace_params_cb_[frameIdx]);
                    }

                    // ---- trace descriptors + dispatch ----
                    {
                        // T3.2: cascade-0 min-mip 链绑定（cone 命中检测消费者）。
                        // PRIMAL_SDF_CONE_HIT=0 或链未分配时回绑 sdf0 本体——
                        // shader 用 textureSize(mip) < textureSize(sdf0) 自门控。
                        static const bool coneHitOn = [] {
                            const char* e = std::getenv("PRIMAL_SDF_CONE_HIT");
                            return !(e && std::strcmp(e, "0") == 0);
                        }();
                        const auto& cas0 = gsdf.GetCascade(0);
                        ResourceHandle mipTex[3];
                        for (u32 m = 0; m < 3; ++m) {
                            // Offline SDF has no mip chain — binding the same
                            // texture makes textureSize(mip)==textureSize(sdf0)
                            // self-gate the cone check off (see shader note).
                            ResourceHandle h = (!useOfflineSdf && coneHitOn && m < cas0.mip_count)
                                ? cas0.mip_textures[m] : cas0.sdf_texture;
                            if (h == rhi::handles::INVALID_RESOURCE) h = cas0.sdf_texture;
                            if (useOfflineSdf) h = sc_offline_sdf_texture_;
                            mipTex[m] = h;
                        }
                        DescriptorData ip[12];
                        // C3.5 slice 4: offline SDF (when present) replaces the
                        // GlobalSDF cascade bindings for the tile march.
                        const rhi::ResourceHandle tileSdf0 = useOfflineSdf
                            ? sc_offline_sdf_texture_ : gsdf.GetCascade(0).sdf_texture;
                        if (device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
                            ip[0] = {0, DescriptorType::StorageImage,  depth_atlas_};
                            ip[1] = {1, DescriptorType::StorageImage,  normal_atlas_};
                            ip[2] = {2, DescriptorType::SampledImage,  tileSdf0};
                            ip[3] = {3, DescriptorType::SampledImage,  useOfflineSdf ? tileSdf0 : gsdf.GetCascade(1).sdf_texture};
                            ip[4] = {4, DescriptorType::SampledImage,  useOfflineSdf ? tileSdf0 : gsdf.GetCascade(2).sdf_texture};
                            ip[5] = {5, DescriptorType::UniformBuffer, indirect_dummy_cb_};
                            ip[6] = {6, DescriptorType::UniformBuffer, indirect_trace_params_cb_[frameIdx]};
                            ip[7] = {7, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()};
                            ip[8] = {8, DescriptorType::StorageBuffer, ray_hits_buffer_};
                            ip[9]  = {9,  DescriptorType::SampledImage, mipTex[0]};
                            ip[10] = {10, DescriptorType::SampledImage, mipTex[1]};
                            ip[11] = {11, DescriptorType::SampledImage, mipTex[2]};
                        } else {
                            ip[0] = {0, DescriptorType::StorageImage,  depth_atlas_};
                            ip[1] = {1, DescriptorType::StorageImage,  normal_atlas_};
                            // 2026-09-11 Metal sync: offline SDF substitution
                            // for the tile march (Vulkan-branch parity) — the
                            // coarse runtime GlobalSDF leaks thin arches as
                            // false sky in the tile traces.
                            ip[2] = {2, DescriptorType::SampledImage,  tileSdf0};
                            ip[3] = {3, DescriptorType::SampledImage,  useOfflineSdf ? tileSdf0 : gsdf.GetCascade(1).sdf_texture};
                            ip[4] = {4, DescriptorType::SampledImage,  useOfflineSdf ? tileSdf0 : gsdf.GetCascade(2).sdf_texture};
                            ip[5] = {5, DescriptorType::SampledImage,  mipTex[0]};
                            ip[6] = {6, DescriptorType::SampledImage,  mipTex[1]};
                            ip[7] = {7, DescriptorType::SampledImage,  mipTex[2]};
                            ip[8] = {0, DescriptorType::UniformBuffer, indirect_dummy_cb_};
                            ip[9] = {1, DescriptorType::UniformBuffer, indirect_trace_params_cb_[frameIdx]};
                            ip[10] = {2, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()};
                            ip[11] = {3, DescriptorType::StorageBuffer, ray_hits_buffer_};
                        }
                        UpdateDescriptorSet(device_, indirect_trace_set_[frameIdx], ip, 12);
                        cmd->BindComputePipeline(indirect_trace_pipeline_);
                        const DescriptorSetHandle isets[] = { indirect_trace_set_[frameIdx] };
                        cmd->BindDescriptorSets(PipelineBindPoint::Compute, indirect_trace_layout_, 0, 1, isets, 0, nullptr);
                        cmd->Dispatch(kTilesEff, kTilesEff, 1);
                        rhi::ResourceBarrier rb{};
                        rb.resource = ray_hits_buffer_;
                        rb.beforeState = rhi::ResourceState::UnorderedAccess;
                        rb.afterState  = rhi::ResourceState::UnorderedAccess;
                        rb.subresource = 0xFFFFFFFF;
                        cmd->InsertBarrier(&rb, 1);

                        // T3.2 bring-up: PRIMAL_SDF_CONE_STATS=1 — 头 3 帧
                        // readback ray_hits 类型分布。与 PRIMAL_SDF_CONE_HIT=0
                        // 对跑，far 计数差 = cone 检测的 sky→far 转化量。
                        //（ProbeRayHit 双端都是 32B/条，hit_type 在 +20B。）
                        static const bool coneStatsOn = [] {
                            const char* e = std::getenv("PRIMAL_SDF_CONE_STATS");
                            return e && atoi(e) != 0;
                        }();
                        static u32 coneStatsFrames = 0;
                        if (coneStatsOn && coneStatsFrames < 3 &&
                            ray_hits_buffer_ != handles::INVALID_RESOURCE) {
                            ++coneStatsFrames;
                            const u64 totalHits = ray_hits_capacity_;
                            const u64 copyBytes = totalHits * 32ull;
                            static rhi::ResourceHandle coneStaging = rhi::handles::INVALID_RESOURCE;
                            if (coneStaging == rhi::handles::INVALID_RESOURCE) {
                                rhi::BufferDesc cs{};
                                cs.size = copyBytes;
                                cs.bindFlags = (u32)rhi::BufferUsageFlags::TransferDst;
                                cs.usage = rhi::GPUMemoryUsage::Readback;
                                cs.memoryUsage = rhi::GPUMemoryUsage::Readback;
                                cs.name = "SDFCone_Stats";
                                coneStaging = device_->CreateBuffer(cs);
                            }
                            if (coneStaging != rhi::handles::INVALID_RESOURCE) {
                                auto ch = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
                                auto* c = rhi::GetCommandBuffer(ch);
                                if (c) {
                                    c->Begin();
                                    c->CopyBuffer(ray_hits_buffer_, coneStaging, 0, 0, copyBytes);
                                    c->End(); c->Submit(); c->WaitForCompletion();
                                    device_->DestroyCommandBuffer(ch);
                                    if (auto* q = static_cast<const u8*>(device_->MapBuffer(coneStaging))) {
                                        u64 nearHits = 0, farHits = 0, skyHits = 0, coneHits = 0;
                                        for (u64 h = 0; h < totalHits; ++h) {
                                            const u32 type = *reinterpret_cast<const u32*>(q + h * 32ull + 20ull);
                                            if (type == 0u) ++nearHits;
                                            else if (type == 1u) ++farHits;
                                            else if (type == 3u) ++coneHits;   // D: cone-occluded
                                            else if (type == 2u) ++skyHits;
                                        }
                                        PRIMAL_LOG_STREAM(Info, "Render")
                                            << "[SDFCone] frame " << coneStatsFrames
                                            << " ray types: near=" << nearHits
                                            << " far=" << farHits
                                            << " sky=" << skyHits
                                            << " cone=" << coneHits
                                            << " (total=" << totalHits << ")" << std::endl;
                                        PRIMAL_LOG_STREAM(Info, "Render")
                                            << "[SDFCone] cas0 origin=(" << cas0.origin.x << ","
                                            << cas0.origin.y << "," << cas0.origin.z << ") extent=("
                                            << cas0.extent.x << "," << cas0.extent.y << ","
                                            << cas0.extent.z << ") voxel=" << cas0.voxel_size
                                            << " mip_count=" << cas0.mip_count << std::endl;
                                        device_->UnmapBuffer(coneStaging);
                                    }
                                }
                            }
                        }
                    }

                    // ---- resolve params upload ----
                    if (auto* rp = static_cast<IndirectResolveParamsGPU*>(
                            device_->MapBuffer(indirect_resolve_params_cb_[frameIdx]))) {
                        *rp = IndirectResolveParamsGPU{};
                        rp->sc_params.atlas_size = atlas_size_;
                        rp->sc_params.page_size = page_size_;
                        rp->sc_params.max_cards = config_.surface_cache_max_cards;
                        rp->sc_params.lookup_count = card_generator_.GetLookupCount();
                        rp->sc_params.indirect_rays_per_probe = kRays;
                        rp->tile_size = kTilesEff != 0u ? kTile : 0u;
                        rp->rays_per_probe = kRays;
                        rp->temporal_weight = 0.9f;
                        rp->frame_index = current_frame_index;
                        rp->lookup_count = card_generator_.GetLookupCount();
                        // B: resolve must mirror the trace's refresh schedule —
                        // tiles outside this frame's phase carry history (if
                        // valid) or explicit zeros, never stale ray_hits.
                        rp->refresh_stride = kStride;
                        rp->refresh_phase = kStride > 1u ? current_frame_index % kStride : 0u;
                        rp->recon_enabled = reconActive ? 1u : 0u;
                        rp->card_count = (u32)card_generator_.GetCards().size();
                        // C: same hit band the trace marches with — a march
                        // hit can undershoot the true surface by up to this
                        // much along the ray, so the capture-validation
                        // tolerance must absorb it.
                        float voxel0 = useOfflineSdf
                            ? sc_offline_sdf_extent_.x / float(std::max(sc_offline_sdf_resolution_, 1u))
                            : (gsdf.GetConfig().cascade_count > 0u
                                ? gsdf.GetCascade(0).extent.x / float(std::max(gsdf.GetCascade(0).resolution, 1u))
                                : 0.30f);
                        rp->hit_band = std::max(voxel0 * 1.25f, 0.30f);
                        device_->UnmapBuffer(indirect_resolve_params_cb_[frameIdx]);
                    }

                    // ---- resolve descriptors + dispatch ----
                    {
                        // P0-B: five of the bindings below are SampledImage,
                        // but the surrounding code parks those atlases in
                        // GENERAL for storage access (material atlases) or
                        // leaves the history slots UNDEFINED on early frames.
                        // Descriptors record SHADER_READ_ONLY_OPTIMAL at write
                        // time, so transition the sampled set here; the backend
                        // resolves oldLayout from the tracked layout, mirroring
                        // the 3.4/3.4-LightEval barrier pattern above.
                        // The two write slots (outIdx) get the inverse UA
                        // transition: with the 3-slot rotation a slot that was
                        // sampled as history last frame is tracked SRO while
                        // this dispatch writes it as StorageImage (GENERAL).
                        {
                            ResourceBarrier resolveBarriers[8]{};
                            resolveBarriers[0].resource = lighting_atlas_[histIdx];
                            resolveBarriers[1].resource = depth_atlas_;
                            resolveBarriers[2].resource = normal_atlas_;
                            resolveBarriers[3].resource = indirect_out_tex_[histIdx];
                            resolveBarriers[4].resource = indirect_tile_tex_[histIdx];
                            for (u32 bi = 0; bi < 5; ++bi) {
                                resolveBarriers[bi].beforeState = ResourceState::ShaderResource;
                                resolveBarriers[bi].afterState  = ResourceState::ShaderResource;
                                resolveBarriers[bi].subresource = 0xFFFFFFFF;
                            }
                            resolveBarriers[5].resource = indirect_out_tex_[outIdx];
                            resolveBarriers[6].resource = indirect_tile_tex_[outIdx];
                            // tile_aux 也在 resolve 的 storage 写列表（rp2[14]）：
                            // 首帧 tracked=UNDEF，写前必须转 GENERAL。
                            resolveBarriers[7].resource = indirect_tile_aux_;
                            for (u32 bi = 5; bi < 8; ++bi) {
                                resolveBarriers[bi].beforeState = ResourceState::UnorderedAccess;
                                resolveBarriers[bi].afterState  = ResourceState::UnorderedAccess;
                                resolveBarriers[bi].subresource = 0xFFFFFFFF;
                            }
                            cmd->InsertBarrier(resolveBarriers, 7);
                        }
                        DescriptorData rp2[16];
                        if (device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
                            rp2[0] = {0, DescriptorType::SampledImage,  lighting_atlas_[histIdx]};
                            rp2[1] = {1, DescriptorType::StorageImage,  albedo_atlas_};
                            rp2[2] = {2, DescriptorType::SampledImage,  indirect_dummy_tex_};   // dummy (unused)
                            rp2[3] = {3, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]};
                            rp2[4] = {4, DescriptorType::UniformBuffer, indirect_dummy_cb_};
                            rp2[5] = {5, DescriptorType::UniformBuffer, indirect_resolve_params_cb_[frameIdx]};
                            rp2[6] = {6, DescriptorType::StorageBuffer, card_generator_.GetCardLookupBuffer()};
                            rp2[7] = {7, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()};
                            rp2[8] = {8, DescriptorType::StorageBuffer, ray_hits_buffer_};
                            rp2[9] = {9, DescriptorType::SampledImage,  indirect_out_tex_[histIdx]};   // T3.3 EMA history
                            // B: per-tile validity bitmask (resolve writes the
                            // bit for tiles it consumed current hits for; the
                            // INVALID stand-in keeps the binding valid when
                            // tile radiosity is resourced off).
                            rp2[10] = {10, DescriptorType::StorageBuffer,
                                       indirect_tile_valid_buffer_ != handles::INVALID_RESOURCE
                                           ? indirect_tile_valid_buffer_
                                           : card_dispatch_buffer_};
                            // C: capture validation + orientation atlases.
                            rp2[11] = {11, DescriptorType::SampledImage, depth_atlas_};
                            rp2[12] = {12, DescriptorType::SampledImage, normal_atlas_};
                            // A: tile-grid output + aux evidence + EMA history.
                            rp2[13] = {13, DescriptorType::StorageImage, indirect_tile_tex_[outIdx]};
                            rp2[14] = {14, DescriptorType::StorageImage, indirect_tile_aux_};
                            rp2[15] = {15, DescriptorType::SampledImage, indirect_tile_tex_[histIdx]};
                        } else {
                            rp2[0] = {0, DescriptorType::SampledImage,  lighting_atlas_[histIdx]};
                            rp2[1] = {1, DescriptorType::StorageImage,  albedo_atlas_};
                            rp2[2] = {2, DescriptorType::SampledImage,  indirect_dummy_tex_};   // dummy (unused)
                            rp2[3] = {3, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]};
                            rp2[4] = {0, DescriptorType::UniformBuffer, indirect_dummy_cb_};
                            rp2[5] = {1, DescriptorType::UniformBuffer, indirect_resolve_params_cb_[frameIdx]};
                            rp2[6] = {2, DescriptorType::StorageBuffer, card_generator_.GetCardLookupBuffer()};
                            rp2[7] = {3, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()};
                            rp2[8] = {4, DescriptorType::StorageBuffer, ray_hits_buffer_};
                            rp2[9] = {6, DescriptorType::SampledImage,  indirect_out_tex_[histIdx]};   // T3.3 EMA history
                            rp2[10] = {5, DescriptorType::StorageBuffer,   // [[buffer(5)]]
                                       indirect_tile_valid_buffer_ != handles::INVALID_RESOURCE
                                           ? indirect_tile_valid_buffer_
                                           : card_dispatch_buffer_};
                            // C: Metal texture namespace — the layout binding
                            // number IS the MSL [[texture(N)]] index: 7 = depth,
                            // 8 = normal (4/5 would collide with the buffer
                            // namespace's ray_hits/tile_valid entries).
                            rp2[11] = {7, DescriptorType::SampledImage, depth_atlas_};
                            rp2[12] = {8, DescriptorType::SampledImage, normal_atlas_};
                            // A: tile-grid [[texture(9)]], aux [[texture(10)]],
                            // prev tile grid [[texture(11)]].
                            rp2[13] = {9, DescriptorType::StorageImage, indirect_tile_tex_[outIdx]};
                            rp2[14] = {10, DescriptorType::StorageImage, indirect_tile_aux_};
                            rp2[15] = {11, DescriptorType::SampledImage, indirect_tile_tex_[histIdx]};
                        }
                        UpdateDescriptorSet(device_, indirect_resolve_set_[frameIdx], rp2, 16);
                        cmd->BindComputePipeline(indirect_resolve_pipeline_);
                        const DescriptorSetHandle rsets[] = { indirect_resolve_set_[frameIdx] };
                        cmd->BindDescriptorSets(PipelineBindPoint::Compute, indirect_resolve_layout_, 0, 1, rsets, 0, nullptr);
                        cmd->Dispatch(kTilesEff, kTilesEff, 1);
                        rhi::ResourceBarrier rb{};
                        rb.resource = indirect_out_tex_[outIdx];
                        rb.beforeState = rhi::ResourceState::UnorderedAccess;
                        rb.afterState  = rhi::ResourceState::UnorderedAccess;
                        rb.subresource = 0xFFFFFFFF;
                        cmd->InsertBarrier(&rb, 1);

                        // A (plan §三.A): continuous reconstruction — replace the
                        // piecewise-constant tile splat with a validated
                        // bilinear rebuild on the tile-center lattice before
                        // LightEval consumes indirect_out.
                        if (reconActive && kTilesEff != 0u) {
                            rhi::ResourceBarrier tileRb{};
                            tileRb.resource = indirect_tile_tex_[outIdx];
                            tileRb.beforeState = rhi::ResourceState::UnorderedAccess;
                            tileRb.afterState  = rhi::ResourceState::ShaderResource;
                            tileRb.subresource = 0xFFFFFFFF;
                            rhi::ResourceBarrier auxRb{};
                            auxRb.resource = indirect_tile_aux_;
                            auxRb.beforeState = rhi::ResourceState::UnorderedAccess;
                            auxRb.afterState  = rhi::ResourceState::ShaderResource;
                            auxRb.subresource = 0xFFFFFFFF;
                            rhi::ResourceBarrier pair[] = { tileRb, auxRb };
                            cmd->InsertBarrier(pair, 2);

                            DescriptorData rc[7];
                            if (device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
                                rc[0] = {0, DescriptorType::UniformBuffer, indirect_resolve_params_cb_[frameIdx]};
                                rc[1] = {1, DescriptorType::StorageImage,  albedo_atlas_};
                                rc[2] = {2, DescriptorType::SampledImage,  depth_atlas_};
                                rc[3] = {3, DescriptorType::SampledImage,  normal_atlas_};
                                rc[4] = {4, DescriptorType::SampledImage,  indirect_tile_tex_[outIdx]};
                                rc[5] = {5, DescriptorType::SampledImage,  indirect_tile_aux_};
                                rc[6] = {6, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]};
                            } else {
                                rc[0] = {0, DescriptorType::UniformBuffer, indirect_resolve_params_cb_[frameIdx]};
                                rc[1] = {0, DescriptorType::StorageImage,  albedo_atlas_};
                                rc[2] = {1, DescriptorType::SampledImage,  indirect_tile_tex_[outIdx]};
                                rc[3] = {2, DescriptorType::SampledImage,  indirect_tile_aux_};
                                rc[4] = {3, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]};
                                rc[5] = {4, DescriptorType::SampledImage,  depth_atlas_};
                                rc[6] = {5, DescriptorType::SampledImage,  normal_atlas_};
                            }
                            UpdateDescriptorSet(device_, indirect_reconstruct_set_[frameIdx], rc, 7);
                            cmd->BindComputePipeline(indirect_reconstruct_pipeline_);
                            const DescriptorSetHandle rcSets[] = { indirect_reconstruct_set_[frameIdx] };
                            cmd->BindDescriptorSets(PipelineBindPoint::Compute, indirect_reconstruct_layout_, 0, 1, rcSets, 0, nullptr);
                            cmd->Dispatch(atlas_size_ / 8u, atlas_size_ / 8u, 1);
                            // tile textures back to UAV for next frame's resolve
                            // write; indirect_out stays UAV → LightEval read.
                            rhi::ResourceBarrier tileRb2{};
                            tileRb2.resource = indirect_tile_tex_[outIdx];
                            tileRb2.beforeState = rhi::ResourceState::ShaderResource;
                            tileRb2.afterState  = rhi::ResourceState::UnorderedAccess;
                            tileRb2.subresource = 0xFFFFFFFF;
                            rhi::ResourceBarrier auxRb2{};
                            auxRb2.resource = indirect_tile_aux_;
                            auxRb2.beforeState = rhi::ResourceState::ShaderResource;
                            auxRb2.afterState  = rhi::ResourceState::UnorderedAccess;
                            auxRb2.subresource = 0xFFFFFFFF;
                            rhi::ResourceBarrier back[] = { tileRb2, auxRb2 };
                            cmd->InsertBarrier(back, 2);
                        }
                    }
                }   // end SC_INDIRECT bring-up block

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

            // ================================================================
            // 4b. P0.1 (00344):material atlases UA -> SR for same-frame
            //     sampled consumers (DDGI finalize samples depth_atlas;
            //     SC↔probe 路径采样 albedo/normal)。SC 全程以 storage 写
            //     (GENERAL),不转 SR 则采样描述符与实际布局冲突。下一帧
            //     开头的 :1759 UA→UA 屏障按跟踪布局(SR→GENERAL)真实过渡,
            //     读写闭环成立。
            // ================================================================
            {
                ResourceBarrier atlasSrBarriers[4]{};
                atlasSrBarriers[0].resource = albedo_atlas_;
                atlasSrBarriers[1].resource = normal_atlas_;
                atlasSrBarriers[2].resource = depth_atlas_;
                atlasSrBarriers[3].resource = emissive_atlas_;
                for (auto& b : atlasSrBarriers) {
                    b.beforeState = ResourceState::UnorderedAccess;
                    b.afterState  = ResourceState::ShaderResource;
                    b.subresource = 0xFFFFFFFF;
                }
                cmd->InsertBarrier(atlasSrBarriers, 4);
            }
        }
    );

    return output;
}

} // namespace primal::graphics::lumen
