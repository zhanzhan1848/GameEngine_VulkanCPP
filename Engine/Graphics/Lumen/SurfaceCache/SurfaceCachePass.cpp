#include "Graphics/RenderGraph/LumenCompareCapture.h"
#include "OwnerCardRanges.h"
#include "BoundaryLightingPoints.h"
#include "CardTileOwners.h"
#include "StaticSurfacePool.h"
#include "TriangleSupportIndex.h"
#include "GeometrySurfaceResample.h"
#include "Graphics/Utilities/GeometryBVHStore.h"
#include "Engine/Graphics/RHI/Core/CardSurfaceIdentity.h"
#include "SurfaceCachePass.h"
#include "Engine/Graphics/Lumen/GIWarmup.h"
#include "Graphics/Lumen/GIDebugParams.h"
#include <cstdlib>
#include <chrono>
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
#include <cmath>
// R3（Windows 首编 IWYU）：std::string/pair/vector 此前靠 libc++ 传递链可见，
// MSVC STL 传递面不同会 C2039——使用点直接补齐（同 TU 顺序两端一致，macOS 零变化）。
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <limits>
#include "Engine/Common/CoreLogStream.h"
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanTexture.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

namespace primal::graphics::lumen {

using namespace rhi;
using namespace rendergraph;
namespace rhimath = primal::graphics::rhi::math;

// Exact opt-in, diagnostic-only reproduction of the former surface policy.
static bool LegacySurfacePolicy() {
    static const bool legacy = [] {
        const char* value = std::getenv("PRIMAL_SC_LEGACY_SURFACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return legacy;
}

bool SurfaceCachePass::HasOfflineSDFSource() const {
    return sc_offline_sdf_texture_ != handles::INVALID_RESOURCE &&
        sc_offline_sdf_resolution_ > 0 &&
        std::isfinite(sc_offline_sdf_origin_.x) && std::isfinite(sc_offline_sdf_origin_.y) &&
        std::isfinite(sc_offline_sdf_origin_.z) &&
        std::isfinite(sc_offline_sdf_extent_.x) && sc_offline_sdf_extent_.x > 0 &&
        std::isfinite(sc_offline_sdf_extent_.y) && sc_offline_sdf_extent_.y > 0 &&
        std::isfinite(sc_offline_sdf_extent_.z) && sc_offline_sdf_extent_.z > 0;
}


// Diagnostic labels only: preserve dispatch, barriers, resources and shader bytes.
// Native profilers may retain these as encoder labels or debug-group events.
static void DispatchScNativeLabel(RHIDeviceBase* device, RHICommandBuffer* cmd,
                                  const char* label, u32 x, u32 y, u32 z) {
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
    static const bool requested = [] {
        const char* value = std::getenv("SC_NATIVE_LABELS");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (requested && device->GetPlatform() == RHIPlatform::Vulkan) {
        auto* vk = static_cast<VulkanDevice*>(device);
        auto begin = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(vk->GetNativeDevice(), "vkCmdBeginDebugUtilsLabelEXT"));
        auto end = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(vk->GetNativeDevice(), "vkCmdEndDebugUtilsLabelEXT"));
        if (begin && end) {
            VkDebugUtilsLabelEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
            info.pLabelName = label;
            auto native = static_cast<VulkanCommandBuffer*>(cmd)->GetNativeCmdBuffer();
            begin(native, &info);
            cmd->Dispatch(x, y, z);
            end(native);
            return;
        }
        static bool warned = false;
        if (!warned) {
            warned = true;
            PRIMAL_LOG_STREAM(Warn, "Render") << "[SC_NATIVE_LABELS] debug utils unavailable" << std::endl;
        }
    }
#endif
    cmd->Dispatch(x, y, z);
}


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

void SurfaceCachePass::ResetSurfaceAtlas(RHICommandBuffer* cmd, u32 slot) {
    if (!cmd || atlas_init_pipeline_ == handles::INVALID_PIPELINE ||
        atlas_reset_cb_ == handles::INVALID_RESOURCE) return;
    if (atlas_seeded_ && surface_seed_epoch_ == card_generator_.GetAllocationEpoch()) return;
    ResourceBarrier barriers[4]{};
    const ResourceHandle images[] = {albedo_atlas_, normal_atlas_, depth_atlas_, emissive_atlas_};
    for (u32 i = 0; i < 4; ++i) {
        barriers[i].resource = images[i];
        barriers[i].beforeState = ResourceState::Unknown;
        barriers[i].afterState = ResourceState::UnorderedAccess;
        barriers[i].subresource = 0xffffffffu;
    }
    cmd->InsertBarrier(barriers, 4);
    const DescriptorData params[] = {
        {0, DescriptorType::StorageImage, albedo_atlas_},
        {1, DescriptorType::StorageImage, normal_atlas_},
        {2, DescriptorType::StorageImage, depth_atlas_},
        {3, DescriptorType::StorageImage, emissive_atlas_},
        {4, DescriptorType::UniformBuffer, atlas_reset_cb_},
        {5, DescriptorType::StorageBuffer, GetCardDispatchBuffer(slot)},
        {6, DescriptorType::StorageBuffer, triangle_atlas_buffer_},
    };
    UpdateDescriptorSet(device_, atlas_init_set_[slot], params, 7);
    cmd->BindComputePipeline(atlas_init_pipeline_);
    cmd->BindDescriptorSets(PipelineBindPoint::Compute, atlas_init_layout_, 0, 1,
                           &atlas_init_set_[slot], 0, nullptr);
    cmd->Dispatch((atlas_size_ * atlas_size_ + 255u) / 256u, 1, 1);
    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::AllCommands,
                      AccessFlag::ShaderWrite, AccessFlag::MemoryRead | AccessFlag::MemoryWrite);
    atlas_seeded_ = true;
    surface_seed_epoch_ = card_generator_.GetAllocationEpoch();
    RequestTileValidityReset();
}


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
        instance_emissive_cpu_[instance_id] = m[instance_id];
        surface_seed_epoch_ = static_composite_epoch_ = page_clear_epoch_ = 0xFFFFFFFFu;
        RequestTileValidityReset();
        device_->UnmapBuffer(instance_emissive_buffer_);
    }
}

// ============================================================================
// [SC per-mesh 计划 P2] 静态表面页：sidecar 挂载 + 注册期双摘要键控。
// 合成管线不可用（Metal v1）时 ResolveStaticSlot 恒 INVALID——旗标与合成
// 能力同生同灭，保证不会出现"跳了 capture 却没页可合成"的黑卡状态。
// ============================================================================
bool SurfaceCachePass::AttachStaticPages(const char* path)
{
    boundary_samples_.Detach();
    boundary_radiance_cache_.Clear();
    boundary_atlas_groups_.clear();boundary_atlas_epoch_=UINT32_MAX;static_composite_epoch_=UINT32_MAX;
    if (!static_pages_) static_pages_ = std::make_unique<SurfaceStaticPages>();
    return static_pages_->Attach(path);
}

bool SurfaceCachePass::AttachBoundarySamples(const char* path)
{
    boundary_samples_.Detach();
    boundary_radiance_cache_.Clear();
    boundary_atlas_groups_.clear();boundary_atlas_epoch_=UINT32_MAX;static_composite_epoch_=UINT32_MAX;
    return static_pages_ && boundary_samples_.Attach(path,*static_pages_);
}

uint32_t SurfaceCachePass::ResolveStaticSlot(uint64_t geometry_digest, uint64_t layout_digest, uint64_t material_digest) const
{
    if (!StaticPagesEnabled() || atlas_init_pipeline_ == handles::INVALID_PIPELINE ||
        page_clear_pipeline_ == handles::INVALID_PIPELINE ||
        static_composite_pipeline_ == handles::INVALID_PIPELINE ||
        static_composite_set_ == handles::INVALID_DESCRIPTOR_SET)
    {
        return SurfaceStaticPages::kInvalidSlot;
    }
    float nearDepth = rhi::card_surface_identity::CaptureNear;
    if (const char* e = std::getenv("PRIMAL_SC_CAPTURE_NEAR")) {
        const float v = std::atof(e);
        if (v >= 0.f && v < 1.f) nearDepth = v;
    }
    if (nearDepth != static_pages_->CaptureNear()) return SurfaceStaticPages::kInvalidSlot;
    float density = 64.f;
    if (const char* e = std::getenv("PRIMAL_SC_TEXELS_PER_UNIT")) {
        const float v = std::atof(e);
        if (std::isfinite(v) && v >= 16.f && v <= 256.f) density = v;
    }
    if (std::abs(density - static_pages_->TexelsPerUnit()) > 1e-4f) return SurfaceStaticPages::kInvalidSlot;
    return static_pages_->FindMesh(geometry_digest, layout_digest, material_digest);
}

bool SurfaceCachePass::Initialize(RHIDeviceBase* device, const LumenConfig& config) {
    if (initialized_) return true;

    device_ = device;
    config_ = config;
    const char* slots = std::getenv("PRIMAL_SC_LIGHT_DISPATCH_SLOTS");
    const char* nonblocking = std::getenv("PRIMAL_VK_QUERY_NONBLOCKING");
    experimental_light_dispatch_slots_ = device_->GetPlatform() == RHIPlatform::Vulkan &&
        ((slots && std::strcmp(slots, "1") == 0) ||
         (nonblocking && std::strcmp(nonblocking, "1") == 0));
    // Experimental three-slot candidate is not promoted to default. The
    // measured cross-run image difference did not establish a single cause.
    atlas_size_ = config.surface_cache_atlas_size;
    page_size_ = config.surface_cache_page_size;
    light_eval_rr_cursor_ = 0;
    lighting_atlas_seed_mask_ = 0;
    instance_emissive_cpu_.fill(math::v4{});
    atlas_seeded_ = false;
    surface_seed_epoch_ = static_composite_epoch_ = page_clear_epoch_ = 0xFFFFFFFFu;

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
    if(device_->GetPlatform()==rhi::RHIPlatform::Vulkan && triangle_atlas_buffer_==handles::INVALID_RESOURCE) return false;

    // T8.12: card-view capture — DEFAULT ON (validated 2026-08-28: L_2F
    // 39.4→46.6 median, REDBOX 9.4→16.6, colored bounce visible, 0 reg).
    // SC_CARDVIEW=0 falls back to the legacy screen-space gather.
    // [SC per-mesh 计划 P2] 静态表面页 sidecar（PRIMAL_SC_STATIC_PAGES=<path>，
    // 默认关）。Attach 失败 = 无静态页，全部 mesh 保持 capture 路径（可区分
    // 状态，不静默伪有效——计划 §3.2）。
    if (const char* spEnv = std::getenv("PRIMAL_SC_STATIC_PAGES"); spEnv && spEnv[0]) {
        if (AttachStaticPages(spEnv)) {
            char msg[512];
            std::snprintf(msg, sizeof(msg),
                "[SurfaceCache] PRIMAL_SC_STATIC_PAGES attached: %s (%u meshes, albedo_scale %.3f)",
                spEnv, static_pages_->MeshCount(), static_pages_->AlbedoScale());
            PRIMAL_LOG_STREAM(Error, "Render") << msg << std::endl;
        } else {
            static_pages_->Detach();
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[SurfaceCache] PRIMAL_SC_STATIC_PAGES attach FAILED — keeping capture path: "
                << spEnv << std::endl;
        }
    }
    if (const char* boundary=std::getenv("PRIMAL_SC_BOUNDARY_SAMPLES"); boundary && *boundary) {
        if (AttachBoundarySamples(boundary)) {
            PRIMAL_LOG_STREAM(Info,"Render") << "[SurfaceCache] SCB1 CPU samples attached: "
                << boundary_samples_.MeshCount() << " meshes, " << boundary_samples_.SampleCount()
                << " samples (GPU path inactive)" << std::endl;
        } else {
            PRIMAL_LOG_STREAM(Warn,"Render") << "[SurfaceCache] SCB1 attach rejected; auxiliary samples disabled: "
                << boundary << std::endl;
        }
    }
    const char* cvEnv = std::getenv("SC_CARDVIEW");
    card_view_enabled_ = !(cvEnv && strcmp(cvEnv, "0") == 0);
    const char* ownerEnv = std::getenv("PRIMAL_SC_CAPTURE_OWNER");
    const bool ownerIsolation = !(ownerEnv && std::strcmp(ownerEnv, "0") == 0);
    if (!card_view_enabled_ && (ownerIsolation ||
        (device_->GetPlatform() == RHIPlatform::Vulkan && !LegacySurfacePolicy()))) {
        card_view_enabled_ = true;
        PRIMAL_LOG_STREAM(Warn, "Render") << "[SurfaceCache] owner isolation requires card-view capture; overriding SC_CARDVIEW=0" << std::endl;
    }

    // Exact opt-in gate for the card writer provenance diagnostic.  It is
    // Vulkan-only and is valid only with the default card-view capture path;
    // a GBuffer capture run must not be presented as the same evidence.
    const char* writerEnv = std::getenv("SC_CAPTURE_SOURCE_AUDIT");
    source_writer_audit_requested_ = writerEnv && strcmp(writerEnv, "1") == 0;
    source_writer_audit_enabled_ = false;
    source_writer_audit_failed_ = false;
    source_writer_atlas_in_uav_ = false;
    const char* geometryEnv = std::getenv("SC_CAPTURE_GEOMETRY_AUDIT");
    geometry_audit_requested_ = geometryEnv && strcmp(geometryEnv, "1") == 0;
    geometry_audit_enabled_ = false;
    geometry_audit_failed_ = false;
    geometry_witness_in_uav_ = false;
    for (auto& witness : geometry_witness_)
        witness = handles::INVALID_RESOURCE;
    if (source_writer_audit_requested_) {
        const bool isVk = device_->GetPlatform() == rhi::RHIPlatform::Vulkan;
        if (!isVk || !card_view_enabled_) {
            source_writer_audit_failed_ = true;
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[SC_CAPTURE_SOURCE_AUDIT] requires Vulkan with card-view capture enabled; "
                << "writer diagnostic closed (no fallback)" << std::endl;
        } else {
            const u64 writerBytes = u64(atlas_size_) * atlas_size_ * 16u;
            constexpr u64 kMaxAuditBufferBytes = 256ull * 1024ull * 1024ull;
            if (atlas_size_ == 0u || writerBytes > kMaxAuditBufferBytes) {
                source_writer_audit_failed_ = true;
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[SC_CAPTURE_SOURCE_AUDIT] writer atlas exceeds diagnostic capacity" << std::endl;
            } else {
                TextureDesc writerDesc{};
                writerDesc.size = {atlas_size_, atlas_size_, 1};
                writerDesc.mipLevels = 1;
                writerDesc.arraySize = 1;
                writerDesc.format = DataFormat::RGBA32_UInt;
                writerDesc.type = TextureType::Texture2D;
                writerDesc.usage = TextureUsage::ShaderResource |
                    TextureUsage::UnorderedAccess | TextureUsage::RenderTarget |
                    TextureUsage::CopySource;
                writerDesc.name = "SurfaceCache_CaptureWriter_RGBA32UInt";
                source_writer_atlas_ = device_->CreateTexture(writerDesc);

                // This is intentionally an off-frame blocking initialization.
                // The RHI contract rejects unsupported backends/call sites;
                // failure closes the audit instead of leaving stale tags.
                if (source_writer_atlas_ == handles::INVALID_RESOURCE) {
                    source_writer_audit_failed_ = true;
                } else {
                    std::vector<u32> zero(writerBytes / sizeof(u32), 0u);
                    TextureRegionUpdate region{};
                    region.width = atlas_size_;
                    region.height = atlas_size_;
                    region.depth = 1;
                    if (!device_->UpdateTextureRegion(source_writer_atlas_, zero.data(),
                                                      writerBytes, region)) {
                        source_writer_audit_failed_ = true;
                        device_->DestroyTexture(source_writer_atlas_);
                        source_writer_atlas_ = handles::INVALID_RESOURCE;
                        PRIMAL_LOG_STREAM(Error, "Render")
                            << "[SC_CAPTURE_SOURCE_AUDIT] blocking writer atlas clear failed"
                            << std::endl;
                    }
                }
            }
        }
    }
    if (geometry_audit_requested_) {
        const bool isVk = device_->GetPlatform() == rhi::RHIPlatform::Vulkan;
        const auto& caps = device_->GetDeviceInfo();
        const u64 witnessBytes = u64(atlas_size_) * atlas_size_ * 16u;
        const u64 aggregateBytes = u64(witnessBytes) * 4u +
                                    (u64(atlas_size_) * atlas_size_ * 16u);
        constexpr u64 kMaxWitnessTextureBytes = 256ull * 1024ull * 1024ull;
        constexpr u64 kMaxGeometryAggregateBytes = 384ull * 1024ull * 1024ull;
        const bool prerequisites = source_writer_audit_requested_ && isVk &&
            card_view_enabled_ && caps.maxRenderTargets >= 8u &&
            caps.maxTexture2DSize >= atlas_size_ && atlas_size_ > 0u &&
            witnessBytes > 0u && witnessBytes <= kMaxWitnessTextureBytes &&
            aggregateBytes <= kMaxGeometryAggregateBytes &&
            source_writer_atlas_ != handles::INVALID_RESOURCE;
        if (!prerequisites) {
            geometry_audit_failed_ = true;
            if (source_writer_audit_requested_) {
                source_writer_audit_failed_ = true;
                source_writer_audit_enabled_ = false;
            }
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[SC_CAPTURE_GEOMETRY_AUDIT] requires source writer/Vulkan/card-view, "
                << "8 MRTs, texture cap, and geometry capacity; diagnostic closed" << std::endl;
        } else {
            const char* names[4] = {
                "SurfaceCache_CaptureGeometryWorldPoint_RGBA32UInt",
                "SurfaceCache_CaptureGeometryTriangle0_RGBA32UInt",
                "SurfaceCache_CaptureGeometryTriangle1_RGBA32UInt",
                "SurfaceCache_CaptureGeometryTriangle2_RGBA32UInt" };
            bool ok = true;
            for (u32 i = 0; i < 4; ++i) {
                TextureDesc witnessDesc{};
                witnessDesc.size = {atlas_size_, atlas_size_, 1};
                witnessDesc.mipLevels = 1;
                witnessDesc.arraySize = 1;
                witnessDesc.format = DataFormat::RGBA32_UInt;
                witnessDesc.type = TextureType::Texture2D;
                witnessDesc.usage = TextureUsage::ShaderResource |
                    TextureUsage::UnorderedAccess | TextureUsage::RenderTarget |
                    TextureUsage::CopySource;
                witnessDesc.name = names[i];
                geometry_witness_[i] = device_->CreateTexture(witnessDesc);
                u32 actualW = 0, actualH = 0;
                if (geometry_witness_[i] == handles::INVALID_RESOURCE ||
                    device_->GetTextureFormat(geometry_witness_[i]) != DataFormat::RGBA32_UInt ||
                    !device_->GetTextureSize(geometry_witness_[i], actualW, actualH) ||
                    actualW != atlas_size_ || actualH != atlas_size_) {
                    ok = false;
                    break;
                }
                std::vector<u32> zero(witnessBytes / sizeof(u32), 0u);
                TextureRegionUpdate region{};
                region.width = atlas_size_;
                region.height = atlas_size_;
                region.depth = 1;
                if (!device_->UpdateTextureRegion(geometry_witness_[i], zero.data(),
                                                  witnessBytes, region)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                geometry_audit_failed_ = true;
                source_writer_audit_failed_ = true;
                source_writer_audit_enabled_ = false;
                for (auto& witness : geometry_witness_) {
                    if (witness != handles::INVALID_RESOURCE)
                        device_->DestroyTexture(witness);
                    witness = handles::INVALID_RESOURCE;
                }
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[SC_CAPTURE_GEOMETRY_AUDIT] witness format/extent/zero initialization failed; "
                    << "diagnostic closed" << std::endl;
            } else {
                geometry_audit_enabled_ = true;
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[SC_CAPTURE_GEOMETRY_AUDIT] four RGBA32_UInt witness atlases allocated and cleared"
                    << std::endl;
            }
        }
    }
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
    if (device_->GetPlatform() == RHIPlatform::Vulkan &&
        !LegacySurfacePolicy() && atlas_reset_cb_ == handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] surface reset CB unavailable" << std::endl;
        return false;
    }

    // Create descriptor set layouts
    CreateDescriptorSetLayouts();

    // Create pipelines and triple-buffered descriptor sets
    CreatePipelines();
    if (device_->GetPlatform() == RHIPlatform::Vulkan && !LegacySurfacePolicy()) {
        bool resetReady = atlas_init_pipeline_ != handles::INVALID_PIPELINE &&
            atlas_init_layout_ != handles::INVALID_PIPELINE_LAYOUT;
        for (const auto set : atlas_init_set_) resetReady &= set != handles::INVALID_DESCRIPTOR_SET;
        if (!resetReady) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] qualified surface reset pipeline unavailable" << std::endl;
            return false;
        }
    }
    card_generator_.SetCoveragePackingDefault(geometry_first_requested_ && StaticPagesEnabled());

    // [SC per-mesh P4] 射线 lookup 分类统计缓冲。
    CreateRayLookupStats();

    initialized_ = true;

    PRIMAL_LOG_STREAM(Info, "Render") << "[SurfaceCache] Initialized (atlas: "
              << atlas_size_ << "x" << atlas_size_
              << ", page: " << page_size_
              << ", max_cards: " << config.surface_cache_max_cards << ")" << std::endl;
    return true;
}

void SurfaceCachePass::Shutdown() {
    boundary_samples_.Detach();
    boundary_radiance_cache_.Clear();
    boundary_atlas_groups_.clear();boundary_atlas_epoch_=UINT32_MAX;
    for(u32 slot=0;slot<3;++slot){boundary_proof_slot_=slot;ReleaseBoundaryLightingProof();}
    boundary_proof_slot_=0;
    if (!initialized_) return;
    initialized_ = false;

    for (auto& b : light_info_buffer_) {
        if (b != handles::INVALID_RESOURCE) device_->DestroyBuffer(b);
        b = handles::INVALID_RESOURCE;
    }
    for (auto& b : card_dispatch_buffer_) {
        if (b != handles::INVALID_RESOURCE) device_->DestroyBuffer(b);
        b = handles::INVALID_RESOURCE;
    }
    for (auto& serial : light_dispatch_consumed_serial_) serial = 0;
    last_light_info_slot_ = 0;
    experimental_light_dispatch_slots_ = false;

    card_view_depth_ = rhi::handles::INVALID_RESOURCE;
    for (auto* resource : {&static_composite_info_buffer_, &static_composite_pool_buffer_, &ray_lookup_stats_buffer_, &page_clear_rects_buffer_, &triangle_atlas_buffer_, &triangle_support_buffer_, &ray_triangle_buffer_}) {
        if (*resource != handles::INVALID_RESOURCE) device_->DestroyBuffer(*resource);
        *resource = handles::INVALID_RESOURCE;
    }
    triangle_support_word_count_=0u;
    for(auto& slot:geometry_buffers_) {
        for(auto& b:slot.buffers) { if(b!=handles::INVALID_RESOURCE) device_->DestroyBuffer(b); b=handles::INVALID_RESOURCE; }
        if(slot.upper_buffer!=handles::INVALID_RESOURCE)device_->DestroyBuffer(slot.upper_buffer);
        slot.upper_buffer=handles::INVALID_RESOURCE;slot.upper_size=0;slot.upper_ready=false;
        if(slot.equal_box_buffer!=handles::INVALID_RESOURCE)device_->DestroyBuffer(slot.equal_box_buffer);
        slot.equal_box_buffer=handles::INVALID_RESOURCE;slot.equal_box_size=0;slot.equal_box_ready=false;slot.equal_box_serial=0;
        slot.epoch=~u64(0); slot.ready=false;
    }
    hybrid_certified_=false;hybrid_field_.clear();for(auto& b:hybrid_bindings_)b={};
    geometry_scene_={}; geometry_equal_box_.reset(); geometry_compact_={}; geometry_upper_nodes_.clear();geometry_upper_stats_={};
    for (auto& b : indirect_tile_owner_buffers_) {
        if (b != handles::INVALID_RESOURCE) device_->DestroyBuffer(b);
        b = handles::INVALID_RESOURCE;
    }
    for (auto& s : indirect_tile_owner_signatures_) s = ~u64(0);
    for (auto& s : indirect_tile_owner_bytes_) s = 0;
    for (auto pipeline : {static_composite_pipeline_, page_clear_pipeline_})
        if (pipeline != handles::INVALID_PIPELINE) device_->DestroyPipeline(pipeline);
    for (auto layout : {static_composite_layout_, page_clear_layout_})
        if (layout != handles::INVALID_PIPELINE_LAYOUT) device_->DestroyPipelineLayout(layout);
    for (auto set : {static_composite_set_, page_clear_set_})
        if (set != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(set);
    for (auto layout : {static_composite_set_layout_, page_clear_set_layout_})
        if (layout != handles::INVALID_DESCRIPTOR_SET_LAYOUT) device_->DestroyDescriptorSetLayout(layout);

    static_composite_pipeline_ = page_clear_pipeline_ = handles::INVALID_PIPELINE;
    static_composite_layout_ = page_clear_layout_ = handles::INVALID_PIPELINE_LAYOUT;
    static_composite_set_ = page_clear_set_ = handles::INVALID_DESCRIPTOR_SET;
    static_composite_set_layout_ = page_clear_set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT;

    if(card_fill_audit_.tables_staging!=handles::INVALID_RESOURCE)
        device_->DestroyBuffer(card_fill_audit_.tables_staging);
    if (atlas_reset_cb_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(atlas_reset_cb_);
    atlas_reset_cb_ = handles::INVALID_RESOURCE;
    if (light_writer_snapshot_.staging != handles::INVALID_RESOURCE)
        device_->DestroyBuffer(light_writer_snapshot_.staging);
    light_writer_snapshot_ = {};
    card_fill_audit_ = {};
    card_fill_audit_cards_.clear();

    for (auto cb : card_fill_writer_cb_)
        if (cb != handles::INVALID_RESOURCE) device_->DestroyBuffer(cb);
    for (auto set : card_fill_writer_set_)
        if (set != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(set);
    if (card_fill_writer_pipeline_ != handles::INVALID_PIPELINE)
        device_->DestroyPipeline(card_fill_writer_pipeline_);
    if (card_fill_writer_layout_ != handles::INVALID_PIPELINE_LAYOUT)
        device_->DestroyPipelineLayout(card_fill_writer_layout_);
    if (card_fill_writer_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        device_->DestroyDescriptorSetLayout(card_fill_writer_set_layout_);
    if (source_writer_atlas_ != handles::INVALID_RESOURCE)
        device_->DestroyTexture(source_writer_atlas_);
    for (auto& witness : geometry_witness_) {
        if (witness != handles::INVALID_RESOURCE)
            device_->DestroyTexture(witness);
    }

    // Clear diagnostic handles only after the corresponding destruction calls.
    // The handles are POD values, so clearing them before the checks above would
    // leak the opt-in writer resources during shutdown.
    source_writer_atlas_ = rhi::handles::INVALID_RESOURCE;
    source_writer_audit_enabled_ = false;
    source_writer_audit_requested_ = false;
    source_writer_audit_failed_ = false;
    source_writer_atlas_in_uav_ = false;
    card_fill_writer_pipeline_ = handles::INVALID_PIPELINE;
    card_fill_writer_layout_ = handles::INVALID_PIPELINE_LAYOUT;
    card_fill_writer_set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    for (auto& set : card_fill_writer_set_) set = handles::INVALID_DESCRIPTOR_SET;
    for (auto& cb : card_fill_writer_cb_) cb = handles::INVALID_RESOURCE;
    geometry_audit_requested_ = false;
    geometry_audit_enabled_ = false;
    geometry_audit_failed_ = false;
    geometry_witness_in_uav_ = false;
    for (auto& witness : geometry_witness_)
        witness = handles::INVALID_RESOURCE;

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

    if(device_->GetPlatform()==rhi::RHIPlatform::Vulkan) {
        BufferDesc desc{}; desc.size=u64(atlas_size_)*atlas_size_*sizeof(u32);
        desc.type=BufferType::Structured; desc.memoryUsage=desc.usage=GPUMemoryUsage::Static;
        desc.structured.elementCount=atlas_size_*atlas_size_; desc.structured.elementStride=sizeof(u32);
        triangle_atlas_buffer_=device_->CreateBuffer(desc);
        if(triangle_atlas_buffer_==handles::INVALID_RESOURCE) PRIMAL_LOG_STREAM(Error,"Render") << "[SC_TRIANGLE] atlas allocation failed";
    }

    // Emissive: RGBA16F — RenderTarget for CardCapture
    CreateAtlas("SurfaceCache_Emissive", DataFormat::RGBA16_Float, emissive_atlas_, true);

    // Lighting atlas: triple-buffered (RGBA16F for HDR lighting)
    for (int i = 0; i < 3; ++i) {
        CreateAtlas(
            ("SurfaceCache_Lighting_" + std::to_string(i)).c_str(),
            DataFormat::RGBA16_Float,
            lighting_atlas_[i]);
    }

    // [E0-B 2026-09-21] nodirect 副本 ×3（LightEval 同帧另存 result −
    // direct·ρ/π；SC/SPGI 查询侧用它收敛 indirect-only 查询）。
    for (int i = 0; i < 3; ++i) {
        CreateAtlas(
            ("SurfaceCache_Lighting_NoDirect_" + std::to_string(i)).c_str(),
            DataFormat::RGBA16_Float,
            lighting_nodirect_atlas_[i]);
    }

    // Runtime buffers
    {
        // Light info buffer: light parameters for evaluation
        BufferDesc lightInfoDesc{};
        lightInfoDesc.size = 256 * sizeof(u32) * 4;  // Max 256 lights, 64 bytes each
        lightInfoDesc.type = BufferType::Structured;
        lightInfoDesc.usage = GPUMemoryUsage::Dynamic;
        lightInfoDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        const u32 count = experimental_light_dispatch_slots_ ? 3u : 1u;
        for (u32 i=0; i<count; ++i) light_info_buffer_[i] = device_->CreateBuffer(lightInfoDesc);
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
        const u32 count = experimental_light_dispatch_slots_ ? 3u : 1u;
        for (u32 i=0; i<count; ++i) card_dispatch_buffer_[i] = device_->CreateBuffer(desc);
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

            cbDesc.size = 128;  // sizeof(IndirectResolveParamsGPU) padded (std140 round-up; K-1 tile_sky_color + 稳定性 tile_growth_cap)
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
    if (device_->GetPlatform() == RHIPlatform::Vulkan) {
        BufferDesc reset{};
        reset.size = 512;
        reset.type = BufferType::Constant;
        reset.usage = reset.memoryUsage = GPUMemoryUsage::Dynamic;
        atlas_reset_cb_ = device_->CreateBuffer(reset);
        if (auto* data = device_->MapBuffer(atlas_reset_cb_)) {
            std::memset(data, 0, 512);
            device_->UnmapBuffer(atlas_reset_cb_);
        } else {
            if (atlas_reset_cb_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(atlas_reset_cb_);
            atlas_reset_cb_ = handles::INVALID_RESOURCE;
        }
    }
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
    if (source_writer_audit_requested_ && !source_writer_audit_failed_) {
        // binding15 of SurfaceCacheCardFill_writer is a four-u32 frame tag.
        CreateCBs(card_fill_writer_cb_, 16);
    }
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

// [SC per-mesh 计划 P4] 射线 lookup 分类统计缓冲（host-visible，原子累加；
// 15×u32——布局见 SurfaceCacheIndirectResolve.comp binding 17 注释）。
// PRIMAL_SC_RAY_LOOKUP_STATS=1 时定期读回。
bool SurfaceCachePass::SetGeometryScene(std::span<const ray_geometry::InstanceInput> instances)
{
    ++geometry_epoch_;
    hybrid_certified_=false;hybrid_field_.clear();for(auto& b:hybrid_bindings_)b={};
    geometry_equal_box_.reset();geometry_compact_={}; geometry_upper_nodes_.clear(); geometry_upper_stats_={};
    bool identityValid=true;
    for(const auto& instance:instances)
        if(!instance.mesh || !card_generator_.BindGeometryIdentity(instance.instance_id,instance.mesh->geometry_digest)) {
            identityValid=false;
            PRIMAL_LOG_STREAM(Error,"Render") << "[SC_GEOMETRY_FIRST] BVH/SC geometry identity mismatch instance=" << instance.instance_id;
        }
    bool loaded=false,persisted=false;
    const bool ok=identityValid && ray_geometry::LoadOrBuildSceneBVH(instances,
        ray_geometry::GeometryCacheDirectory(),geometry_scene_,&loaded,&persisted);
    geometry_source_digest_=ok?ray_geometry::SceneSourceDigest(instances):0;
    if(!ok)geometry_scene_={};
    else {
        const char* equalBox=std::getenv("PRIMAL_DDGI_GEOMETRY_EQUAL_BOX");
        if(device_ && device_->GetPlatform()==RHIPlatform::Vulkan && equalBox && std::strcmp(equalBox,"1")==0)
            geometry_equal_box_=ray_geometry::TryBuildEqualBoxSidecar(geometry_scene_,16ull*1024*1024);
        const char* upper=std::getenv("PRIMAL_DDGI_GEOMETRY_UPPER_BVH");
        const char* compact=std::getenv("PRIMAL_DDGI_GEOMETRY_COMPACT_STACKS");
        const char* visibility=std::getenv("PRIMAL_DDGI_GEOMETRY_VISIBILITY");
        const char* reference=std::getenv("PRIMAL_DDGI_GEOMETRY_VISIBILITY_REFERENCE");
        const bool upperRequested=device_ && device_->GetPlatform()==RHIPlatform::Vulkan && upper && std::strcmp(upper,"1")==0 && visibility && std::strcmp(visibility,"1")==0 &&
            !(reference && std::strcmp(reference,"1")==0);
        if(upperRequested) {
            ray_geometry::SceneBVH candidate;
            const bool built=ray_geometry::BuildUpperSAH16(geometry_scene_,candidate,geometry_upper_stats_);
            if(built) geometry_upper_nodes_=std::move(candidate.bottom);
            PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_UPPER_BVH] built=" << built << " roots=" << geometry_upper_stats_.roots_changed
                << " build_ms=" << geometry_upper_stats_.build_ms << " old_cost=" << geometry_upper_stats_.original_cost
                << " new_cost=" << geometry_upper_stats_.candidate_cost << " old_pending=" << geometry_upper_stats_.original_max_pending
                << " new_pending=" << geometry_upper_stats_.candidate_max_pending << " old_depth=" << geometry_upper_stats_.original_max_depth
                << " new_depth=" << geometry_upper_stats_.candidate_max_depth << " bytes_per_slot=" << geometry_upper_stats_.extra_gpu_bytes << std::endl;
        }
        if(!upperRequested && compact && std::strcmp(compact,"1")==0 && visibility && std::strcmp(visibility,"1")==0 &&
           !(reference && std::strcmp(reference,"1")==0)) {
            geometry_compact_=ray_geometry::CertifyCompactTraversal(geometry_scene_);
            PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_COMPACT_STACKS] certificate_valid=" << geometry_compact_.valid
                << " fits=" << geometry_compact_.Fits() << " top_pending=" << geometry_compact_.top_max_pending
                << " bottom_pending=" << geometry_compact_.bottom_max_pending << " scene_epoch=" << geometry_epoch_ << std::endl;
        }
    }
    if(ok && device_ && device_->GetPlatform()==RHIPlatform::Vulkan) {
        const char* hybrid=std::getenv("PRIMAL_ENV_SDF_HYBRID");
        if(hybrid && std::strcmp(hybrid,"1")==0) {
            hybrid_certified_=geometry_source_digest_==environment_hybrid::kSourceDigest && environment_hybrid::Verify(geometry_scene_,hybrid_field_);
            if(!hybrid_certified_)hybrid_field_.clear();
            PRIMAL_LOG_STREAM(Info,"Render") << "[ENV_FIELD_CERT] epoch=" << geometry_epoch_ << " certified=" << hybrid_certified_;
        }
    }
    if(ok && !persisted)PRIMAL_LOG_STREAM(Warn,"Render") << "[SC_BVH_CACHE] unable to persist scene; using validated memory BVH";
    RequestTileValidityReset();
    PRIMAL_LOG_STREAM(Info,"Render") << "[SC_GEOMETRY_FIRST] scene_complete=" << ok
        << " cache=" << (loaded?"loaded":persisted?"saved":"memory") << " source_digest=" << geometry_source_digest_
        << " instances=" << geometry_scene_.instances.size() << " blas_nodes=" << geometry_scene_.bottom.size()
        << " triangles=" << geometry_scene_.triangles.size();
    return ok;
}

environment_hybrid::Binding SurfaceCachePass::GetHybridBinding(u32 frame) const {
    frame%=3;auto b=hybrid_bindings_[frame];const auto& s=geometry_buffers_[frame];
    b.ready=b.ready && hybrid_certified_ && device_ && device_->FrameSlotCompletions().CanReuse(frame,0) && s.ready && s.epoch==geometry_epoch_ && b.epoch==geometry_epoch_ && b.digest==geometry_source_digest_;
    for(u32 i=0;i<4;++i)b.ready=b.ready && b.handles[i]==s.buffers[i] && b.allocated[i]==s.sizes[i];return b;
}

bool SurfaceCachePass::UploadGeometryScene(u32 frame)
{
    auto& slot=geometry_buffers_[frame];
    if(slot.epoch==geometry_epoch_) {
        // If the ordinary path uploaded before completion was witnessed, H may
        // establish its own proof only by re-uploading after a real slot fence.
        const bool retryHybrid=slot.ready && hybrid_certified_ && hybrid_owner_id_ && !hybrid_bindings_[frame].ready && device_->FrameSlotCompletions().CanReuse(frame,0);
        if(!retryHybrid)return slot.ready;
    }
    const void* data[4]={geometry_scene_.top.data(),geometry_scene_.bottom.data(),
        geometry_scene_.triangles.data(),geometry_scene_.instances.data()};
    const u64 size[4]={geometry_scene_.top.size()*sizeof(ray_geometry::Node),geometry_scene_.bottom.size()*sizeof(ray_geometry::Node),
        geometry_scene_.triangles.size()*sizeof(ray_geometry::Triangle),geometry_scene_.instances.size()*sizeof(ray_geometry::Instance)};
    const bool hybridCompletion=hybrid_certified_ && hybrid_owner_id_ && device_->FrameSlotCompletions().CanReuse(frame,0);
    const u64 hybridCompletionSerial=hybridCompletion?device_->FrameSlotCompletions().Serial(frame):0;
    hybrid_bindings_[frame]={};
    slot.ready=false; slot.upper_ready=false; slot.upper_epoch=~u64(0); slot.upper_digest=0;
    slot.compact={}; slot.compact_epoch=~u64(0); slot.compact_digest=0; slot.compact_slot=~u32(0);
    // Each slot is updated only after the frame fence. Never destroy another
    // in-flight slot when scene geometry or transforms change.
    const u8 zero[64]{};
    bool ok=!geometry_scene_.top.empty();
    for(u32 i=0;i<4;++i) {
        const u64 bytes=std::max<u64>(64,size[i]);
        if(slot.sizes[i]!=bytes || slot.buffers[i]==handles::INVALID_RESOURCE) {
            if(slot.buffers[i]!=handles::INVALID_RESOURCE) device_->DestroyBuffer(slot.buffers[i]);
            BufferDesc desc{}; desc.size=bytes; desc.type=BufferType::Structured;
            desc.usage=desc.memoryUsage=GPUMemoryUsage::Dynamic;
            slot.buffers[i]=device_->CreateBuffer(desc); slot.sizes[i]=bytes;
        }
        auto* dst=slot.buffers[i]!=handles::INVALID_RESOURCE?device_->MapBuffer(slot.buffers[i]):nullptr;
        if(!dst) { ok=false; continue; }
        if(size[i]) std::memcpy(dst,data[i],size[i]); else std::memcpy(dst,zero,64);
        device_->UnmapBuffer(slot.buffers[i]);
    }
    slot.epoch=ok?geometry_epoch_:~u64(0); slot.ready=ok;
    if(ok && hybridCompletion && hybrid_generation_!=~u64(0)) {
        auto& b=hybrid_bindings_[frame];b.producer=this;b.producer_id=hybrid_owner_id_;b.epoch=geometry_epoch_;b.digest=geometry_source_digest_;b.generation=++hybrid_generation_;b.completion=hybridCompletionSerial;b.slot=frame;b.ready=true;
        for(u32 i=0;i<4;++i){b.handles[i]=slot.buffers[i];b.allocated[i]=slot.sizes[i];b.logical[i]=size[i];b.ready=b.ready && size[i]>0 && slot.buffers[i]!=handles::INVALID_RESOURCE;}
    }
    if(ok && geometry_compact_.Fits()) {
        slot.compact=geometry_compact_; slot.compact_epoch=geometry_epoch_;
        slot.compact_digest=geometry_source_digest_; slot.compact_slot=frame;
        for(u32 i=0;i<4;++i){slot.compact_buffers[i]=slot.buffers[i];slot.compact_sizes[i]=slot.sizes[i];}
    }
    if(ok && !geometry_upper_nodes_.empty()) {
        const auto upperStart=std::chrono::steady_clock::now();
        const u64 bytes=geometry_upper_nodes_.size()*sizeof(ray_geometry::Node);
        if(slot.upper_size!=bytes || slot.upper_buffer==handles::INVALID_RESOURCE) {
            if(slot.upper_buffer!=handles::INVALID_RESOURCE)device_->DestroyBuffer(slot.upper_buffer);
            BufferDesc desc{};desc.size=bytes;desc.type=BufferType::Structured;
            desc.usage=desc.memoryUsage=GPUMemoryUsage::Dynamic;
            slot.upper_buffer=device_->CreateBuffer(desc);slot.upper_size=bytes;
        }
        auto* dst=slot.upper_buffer!=handles::INVALID_RESOURCE?device_->MapBuffer(slot.upper_buffer):nullptr;
        if(dst) {
            std::memcpy(dst,geometry_upper_nodes_.data(),bytes);device_->UnmapBuffer(slot.upper_buffer);
            slot.upper_epoch=geometry_epoch_;slot.upper_digest=geometry_source_digest_;slot.upper_slot=frame;
            slot.upper_original_hash=geometry_upper_stats_.original_payload_hash;slot.upper_candidate_hash=geometry_upper_stats_.candidate_payload_hash;
            for(u32 i=0;i<4;++i){slot.upper_source_buffers[i]=slot.buffers[i];slot.upper_source_sizes[i]=slot.sizes[i];}
            slot.upper_ready=true;
        }
        PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_UPPER_BVH] slot=" << frame << " upload_ready=" << slot.upper_ready
            << " bytes=" << bytes << " upload_cpu_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-upperStart).count() << std::endl;
    }
    return ok;
}

bool SurfaceCachePass::EqualBoxSlotComplete(u32 frame) const {
    return device_ && device_->FrameSlotCompletions().CanReuse(frame%3,0);
}
bool SurfaceCachePass::PrepareEqualBoxConsumer(u32 frame) {
    frame%=3;auto& slot=geometry_buffers_[frame];
    if(!device_ || !geometry_equal_box_ || !geometry_equal_box_->valid || !slot.ready || slot.epoch!=geometry_epoch_)return false;
    const auto current=GetGeometryWitnessView(frame);
    if(current.equal_box_ready)return true;
    slot.equal_box_ready=false;
    const auto& completed=device_->FrameSlotCompletions();
    if(!completed.CanReuse(frame,slot.equal_box_serial))return false;
    const u64 serial=completed.Serial(frame),bytes=geometry_equal_box_->flags.size()*sizeof(u32);
    if(!bytes || !ray_geometry::EqualBoxSidecarIntact(*geometry_equal_box_))return false;
    if(slot.equal_box_size!=bytes || slot.equal_box_buffer==handles::INVALID_RESOURCE) {
        if(slot.equal_box_buffer!=handles::INVALID_RESOURCE)device_->DestroyBuffer(slot.equal_box_buffer);
        BufferDesc desc{};desc.size=bytes;desc.type=BufferType::Structured;desc.usage=desc.memoryUsage=GPUMemoryUsage::Dynamic;
        slot.equal_box_buffer=device_->CreateBuffer(desc);slot.equal_box_size=bytes;
    }
    void* dst=slot.equal_box_buffer!=handles::INVALID_RESOURCE?device_->MapBuffer(slot.equal_box_buffer):nullptr;
    if(!dst)return false;
    std::memcpy(dst,geometry_equal_box_->flags.data(),bytes);device_->UnmapBuffer(slot.equal_box_buffer);
    slot.equal_box_epoch=geometry_epoch_;slot.equal_box_digest=geometry_source_digest_;slot.equal_box_slot=frame;
    slot.equal_box_flags_hash=geometry_equal_box_->flags_hash;slot.equal_box_payload_hash=geometry_equal_box_->payload_hash;
    for(u32 i=0;i<4;++i){slot.equal_box_sources[i]=slot.buffers[i];slot.equal_box_source_sizes[i]=slot.sizes[i];}
    slot.equal_box_serial=serial;slot.equal_box_ready=true;
    PRIMAL_LOG_STREAM(Info,"Render") << "[DDGI_EQUAL_BOX] upload_slot=" << frame << " epoch=" << geometry_epoch_ << " serial=" << serial << " bytes=" << bytes << std::endl;
    return GetGeometryWitnessView(frame).equal_box_ready;
}

bool SurfaceCachePass::ReadbackRayLookupStats(uint32_t frame)
{
    if (ray_lookup_stats_buffer_ == handles::INVALID_RESOURCE) return false;
    rhi::BufferDesc rb{};
    const char* auditPrefix = std::getenv("SC_RAY_ORACLE_PREFIX");
    const bool audit = auditPrefix && auditPrefix[0] && device_->GetPlatform() == rhi::RHIPlatform::Vulkan;
    rb.size = (audit ? 16u + 16384u * 16u : 15u) * sizeof(u32); rb.memoryUsage = rhi::GPUMemoryUsage::Readback;
    rb.bindFlags = u32(rhi::BufferUsageFlags::TransferDst);
    auto staging = device_->CreateBuffer(rb);
    auto handle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    auto* cmd = rhi::GetCommandBuffer(handle);
    bool complete = false;
    if (staging != handles::INVALID_RESOURCE && cmd && cmd->Begin()) {
        cmd->MemoryBarrier(rhi::PipelineStage::AllCommands, rhi::PipelineStage::Transfer,
                           rhi::AccessFlag::MemoryWrite, rhi::AccessFlag::TransferRead);
        cmd->CopyBuffer(ray_lookup_stats_buffer_, staging, 0, 0, rb.size);
        cmd->MemoryBarrier(rhi::PipelineStage::Transfer, rhi::PipelineStage::Host,
                           rhi::AccessFlag::TransferWrite, rhi::AccessFlag::HostRead);
        complete = cmd->End() && cmd->Submit() && cmd->WaitForCompletion();
    }
    bool read = false;
    if (complete) {
        if (auto* st = static_cast<const u32*>(device_->MapBuffer(staging))) {
            if (FILE* sf = std::fopen("/tmp/sc_ray_lookup_stats.log", "a")) {
                std::fprintf(sf, "{\"frame\":%u,\"queried\":%u,\"found\":%u,\"d005\":%u,\"d015\":%u,\"d045\":%u,\"dgt\":%u,\"rej\":[%u,%u,%u,%u,%u,%u,%u,%u]}\n",
                    frame, st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9], st[10], st[11], st[12], st[13]);
                std::fclose(sf); read = true;
            }
            if (audit) {
                std::vector<u32> records;
                for (u32 i=0; i<16384; ++i) {
                    const u32* r = st + 16 + i*16;
                    if (r[12] && r[11] <= 2u) records.insert(records.end(),r,r+16);
                }
                const std::string path = std::string(auditPrefix) + ".samples.bin";
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    const u32 h[2] = {u32(records.size()/16),16};
                    std::fwrite(h,sizeof(h),1,f); std::fwrite(records.data(),4,records.size(),f); std::fclose(f);
                }
                const std::string query = std::string(auditPrefix) + ".rays.bin";
                if (FILE* f = std::fopen(query.c_str(), "wb")) {
                    std::fwrite("PGRAYS01",8,1,f); const u32 h[2] = {u32(records.size()/16),0};
                    std::fwrite(h,sizeof(h),1,f);
                    for (size_t i=0; i<records.size(); i+=16) std::fwrite(records.data()+i,4,8,f);
                    std::fclose(f);
                }
            }
            device_->UnmapBuffer(staging);
        }
    }
    if (handle != handles::INVALID_COMMAND_BUFFER) device_->DestroyCommandBuffer(handle);
    if (staging != handles::INVALID_RESOURCE) device_->DestroyBuffer(staging);
    // Optional post-frame snapshot of the exact atlas consumed by the identity
    // resolver. A separate submitted copy preserves normal dispatch behavior.
    if (audit && std::getenv("SC_ATLAS_COVERAGE_AUDIT") &&
        triangle_atlas_buffer_ != handles::INVALID_RESOURCE && !card_generator_.IsPageTableEnabled()) {
        const u64 atlasBytes = u64(atlas_size_) * atlas_size_ * sizeof(u32);
        bool saved = true;
        for (u32 plane = 0; plane < (triangle_support_word_count_ && std::getenv("SC_CARD_TRIANGLE_AUDIT")?3u:2u); ++plane) {
            const u64 bytes=plane==2u?u64(triangle_support_word_count_)*4u:atlasBytes;
            rhi::BufferDesc desc{};
            desc.size = bytes;
            desc.memoryUsage = rhi::GPUMemoryUsage::Readback;
            desc.bindFlags = u32(rhi::BufferUsageFlags::TransferDst);
            auto dest = device_->CreateBuffer(desc);
            auto copyHandle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
            auto* copy = rhi::GetCommandBuffer(copyHandle);
            bool done = false;
            if (dest != handles::INVALID_RESOURCE && copy && copy->Begin()) {
                copy->MemoryBarrier(rhi::PipelineStage::AllCommands, rhi::PipelineStage::Transfer,
                                    rhi::AccessFlag::MemoryWrite, rhi::AccessFlag::TransferRead);
                if (plane == 0) {
                    rhi::ResourceBarrier transition{};
                    transition.resource = depth_atlas_;
                    transition.beforeState = rhi::ResourceState::ShaderResource;
                    transition.afterState = rhi::ResourceState::CopySource;
                    transition.subresource = 0xFFFFFFFFu;
                    copy->InsertBarrier(&transition, 1);
                    rhi::BufferTextureCopyRegion region{};
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = {atlas_size_, atlas_size_, 1};
                    copy->CopyTextureToBuffer(depth_atlas_, dest, &region, 1);
                    transition.beforeState = rhi::ResourceState::CopySource;
                    transition.afterState = rhi::ResourceState::ShaderResource;
                    copy->InsertBarrier(&transition, 1);
                } else {
                    copy->CopyBuffer(plane==2u?triangle_support_buffer_:triangle_atlas_buffer_, dest, 0, 0, bytes);
                }
                copy->MemoryBarrier(rhi::PipelineStage::Transfer, rhi::PipelineStage::Host,
                                    rhi::AccessFlag::TransferWrite, rhi::AccessFlag::HostRead);
                done = copy->End() && copy->Submit() && copy->WaitForCompletion();
            }
            bool written = false;
            if (done) if (auto* data = device_->MapBuffer(dest)) {
                const std::string path = std::string(auditPrefix) + (plane == 0 ? ".atlas-depth.bin" : plane==1?".atlas-ids.bin":".triangle-presence.bin");
                if (FILE* file = std::fopen(path.c_str(), "wb")) {
                    written = std::fwrite(data, 1, bytes, file) == bytes;
                    written = std::fclose(file) == 0 && written;
                }
                device_->UnmapBuffer(dest);
            }
            saved &= written;
            if (copyHandle != handles::INVALID_COMMAND_BUFFER) device_->DestroyCommandBuffer(copyHandle);
            if (dest != handles::INVALID_RESOURCE) device_->DestroyBuffer(dest);
        }
        const auto& cards = card_generator_.GetCards();
        const std::string path = std::string(auditPrefix) + ".atlas-cards.bin";
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
            const u32 header[4] = {atlas_size_, u32(cards.size()), frame, card_generator_.GetAllocationEpoch()};
            saved &= std::fwrite(header, sizeof(header), 1, file) == 1;
            saved &= std::fwrite(cards.data(), sizeof(SurfaceCacheCard), cards.size(), file) == cards.size();
            saved &= std::fclose(file) == 0;
        } else saved = false;
        // The metadata file commits the snapshot; an incomplete copy must
        // never leave a readable header certifying stale planes.
        if (!saved) std::remove(path.c_str());
        PRIMAL_LOG_STREAM(Info, "Render") << "[SC_ATLAS_COVERAGE_AUDIT] complete=" << saved
            << " frame=" << frame << " epoch=" << card_generator_.GetAllocationEpoch();
    }
    return read;
}

void SurfaceCachePass::CreateRayLookupStats()
{
    rhi::BufferDesc sbDesc{};
    const char* audit = std::getenv("SC_RAY_ORACLE_PREFIX");
    const u32 words = audit && audit[0] && device_->GetPlatform() == rhi::RHIPlatform::Vulkan ? 16u + 16384u*16u : 15u;
    sbDesc.size = words * sizeof(uint32_t);
    sbDesc.type = rhi::BufferType::Structured;
    sbDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    sbDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    sbDesc.structured.elementCount = words;
    sbDesc.structured.elementStride = sizeof(uint32_t);
    ray_lookup_stats_buffer_ = device_->CreateBuffer(sbDesc);
    if (ray_lookup_stats_buffer_ != handles::INVALID_RESOURCE) {
        if (auto* z = static_cast<uint32_t*>(device_->MapBuffer(ray_lookup_stats_buffer_))) {
            memset(z, 0, words * sizeof(uint32_t));
            device_->UnmapBuffer(ray_lookup_stats_buffer_);
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
            {15, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // [WRC Stream D2]: 卡片页表（过表寻址）
            {16, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},  // [E0-B] nodirect 副本输出
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[9].readonly = true;
        b[10].readonly = true;
        b[13].readonly = true;
        b[14].readonly = true;
        b[15].readonly = true;
        light_eval_set_layout_ = device_->CreateDescriptorSetLayout({17, bindings});
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
            {7, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr},   // [E0-B] nodirect 副本输出 [[texture(7)]]
        };
        DescriptorSetLayoutBinding* bm = bindings;
        bm[5].readonly = true;
        bm[8].readonly = true;  // ddgi irradiance
        bm[13].readonly = true; // ddgi depth
        bm[14].readonly = true; // GI 根治 Phase 3.2: ddgi openSky
        // 修复：15 个条目（含 openSky buffer(8)）但 count 传 14——最后一个
        // 条目从未进布局，Metal LightEval 编码期断言
        // "missing Buffer binding at index 8 for ddgi_opensky_buffer"。
        // [E0-B] +1：nodirect 副本输出 [[texture(7)]]（纹理命名空间 7 空闲）。
        DescriptorSetLayoutDesc layoutDesc{16, bindings};
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

    geometry_first_requested_=isVk && std::getenv("PRIMAL_SC_GEOMETRY_FIRST") &&
        std::strcmp(std::getenv("PRIMAL_SC_GEOMETRY_FIRST"),"1")==0;

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
            {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // tile owner
            {13, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // instance BVH
            {14, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // mesh BVHs
            {15, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // triangles
            {16, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // inverse transforms
            {17, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // original ray TriangleID
        };
        // Mark SDF textures as 3D view dimension
        bindings[2].is3D = true;
        bindings[3].is3D = true;
        bindings[4].is3D = true;
        bindings[9].is3D = true;
        bindings[10].is3D = true;
        bindings[11].is3D = true;
        indirect_trace_set_layout_ = device_->CreateDescriptorSetLayout({geometry_first_requested_ ? 18u : 13u, bindings});
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
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // tile owner   [[buffer(4)]]
        };
        DescriptorSetLayoutDesc layoutDesc{13, bindings};
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
            {16, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // [WRC Stream D2]: 卡片页表（过表寻址）
            {17, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // [SC per-mesh P4]: 射线 lookup 分类统计（原子累加）
            {18, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // ray triangle
            {19, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // atlas triangle
            {20, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // epoch-local triangle support
        };
        const char* boundaryConsumer=std::getenv("SC_BOUNDARY_CONSUMER");
        const char* continuous=std::getenv("SC_BOUNDARY_CONTINUOUS");
        boundary_continuous_requested_=geometry_first_requested_ && continuous && std::strcmp(continuous,"1")==0;
        boundary_consumer_requested_=boundary_continuous_requested_ || (geometry_first_requested_ && boundaryConsumer && std::strcmp(boundaryConsumer,"1")==0);
        std::vector<DescriptorSetLayoutBinding> resolveBindings(bindings,bindings+(geometry_first_requested_?21u:18u));
        if(boundary_consumer_requested_)for(u32 i=21;i<24;++i)resolveBindings.push_back({i,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr});
        indirect_resolve_set_layout_=device_->CreateDescriptorSetLayout({u32(resolveBindings.size()),resolveBindings.data()});
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
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // atlas TriangleID
        };
        DescriptorSetLayoutBinding* b = bindings;
        b[5].readonly = true;
        atlas_init_set_layout_ = device_->CreateDescriptorSetLayout({7, bindings});
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

    // --- [SC per-mesh P2] StaticComposite: 3 write images + 2 readonly SSBOs
    //     (Vulkan-only; Metal 保持 capture 路径) ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // albedo_atlas
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // normal_atlas
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // depth_atlas
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // StaticCompositeInfo[]
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // 页字节池（uint words）
            {5, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // emissive_atlas
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // atlas TriangleID
        };
        bindings[3].readonly = true;
        bindings[4].readonly = true;
        static_composite_set_layout_ = device_->CreateDescriptorSetLayout({7, bindings});
    }

    // --- [SC per-mesh P2] PageClear: 6 lighting storage images + 1 rects SSRO
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {5, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},
        };
        bindings[6].readonly = true;
        page_clear_set_layout_ = device_->CreateDescriptorSetLayout({7, bindings});
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
            {16, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr}, // invalid emissive
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
        card_fill_set_layout_ = device_->CreateDescriptorSetLayout({15, bindings});

        if (source_writer_audit_requested_ && !source_writer_audit_failed_) {
            // Keep bindings 0..13 byte-for-byte identical to the production
            // layout.  The diagnostic variant appends only the writer image
            // and its frame constant (14/15).
            DescriptorSetLayoutBinding writerBindings[17]{};
            std::memcpy(writerBindings, bindings, sizeof(bindings));
            writerBindings[15] = {14, DescriptorType::StorageImage, 1,
                                  ShaderStage::Compute, nullptr};
            writerBindings[16] = {15, DescriptorType::UniformBuffer, 1,
                                  ShaderStage::Compute, nullptr};
            card_fill_writer_set_layout_ = device_->CreateDescriptorSetLayout({17, writerBindings});
            if (card_fill_writer_set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
                source_writer_audit_failed_ = true;
            }
        }
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
    auto indirectTraceShader  = CompileComputeShader(geometry_first_requested_ ? "SurfaceCacheIndirectTrace_geometry" : "SurfaceCacheIndirectTrace",
        isVk ? "surface_cache_indirect_trace" : "surfaceCacheIndirectTrace");
    auto indirectResolveShader = CompileComputeShader(boundary_consumer_requested_ ? "SurfaceCacheIndirectResolve_consumer" : geometry_first_requested_ ? "SurfaceCacheIndirectResolve_geometry" : "SurfaceCacheIndirectResolve",
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
    ShaderHandle cardFillWriterShader = handles::INVALID_SHADER;
    if (isVk) {
        cardFillShader = CompileComputeShader("SurfaceCacheCardFill", "surface_cache_card_fill");
        if (source_writer_audit_requested_ && !source_writer_audit_failed_) {
            cardFillWriterShader = CompileComputeShader("SurfaceCacheCardFill_writer",
                                                        "surface_cache_card_fill");
            if (cardFillWriterShader == handles::INVALID_SHADER) {
                source_writer_audit_failed_ = true;
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[SC_CAPTURE_SOURCE_AUDIT] CardFill writer shader variant missing"
                    << std::endl;
            }
        }
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

        // [SC per-mesh P2] 静态页合成管线（Vulkan-only）。
        if (isVk) {
            auto staticCompositeShader = CompileComputeShader("SurfaceCacheStaticComposite",
                                                              "surface_cache_static_composite");
            if (staticCompositeShader != handles::INVALID_SHADER &&
                static_composite_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
                ComputePipelineDesc sPipeDesc{};
                sPipeDesc.computeShader = staticCompositeShader;
                PipelineLayoutDesc sPlDesc{};
                sPlDesc.setLayoutCount = 1;
                sPlDesc.setLayouts = &static_composite_set_layout_;
                static_composite_layout_ = device_->CreatePipelineLayout(sPlDesc);
                sPipeDesc.layout = static_composite_layout_;
                static_composite_pipeline_ = device_->CreateComputePipeline(sPipeDesc);
            }
            // [SC per-mesh P2] 挤页 rect 清零管线。
            auto pageClearShader = CompileComputeShader("SurfaceCachePageClear",
                                                        "surface_cache_page_clear");
            if (pageClearShader != handles::INVALID_SHADER &&
                page_clear_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
                ComputePipelineDesc pcPipeDesc{};
                pcPipeDesc.computeShader = pageClearShader;
                PipelineLayoutDesc pcPlDesc{};
                pcPlDesc.setLayoutCount = 1;
                pcPlDesc.setLayouts = &page_clear_set_layout_;
                page_clear_layout_ = device_->CreatePipelineLayout(pcPlDesc);
                pcPipeDesc.layout = page_clear_layout_;
                page_clear_pipeline_ = device_->CreateComputePipeline(pcPipeDesc);
            }
        }
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
    if (isVk && source_writer_audit_requested_ && !source_writer_audit_failed_ &&
        cardFillWriterShader != handles::INVALID_SHADER &&
        card_fill_writer_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PipelineLayoutDesc plDesc{};
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &card_fill_writer_set_layout_;
        card_fill_writer_layout_ = device_->CreatePipelineLayout(plDesc);
        if (card_fill_writer_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
            ComputePipelineDesc writerDesc{};
            writerDesc.computeShader = cardFillWriterShader;
            writerDesc.layout = card_fill_writer_layout_;
            writerDesc.threadGroupSize = {256, 1, 1};
            card_fill_writer_pipeline_ = device_->CreateComputePipeline(writerDesc);
        }
        if (card_fill_writer_pipeline_ == handles::INVALID_PIPELINE) {
            source_writer_audit_failed_ = true;
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[SC_CAPTURE_SOURCE_AUDIT] CardFill writer pipeline creation failed"
                << std::endl;
        }
    }
    // Shadow (T3.3): (256,1,1) — per-texel SDF shadow march toward the light
    {
        const bool traceValidZero = isVk && [] {
            const char* value = std::getenv("SC_SHADOW_TRACE_VALID_ZERO");
            return value && std::atoi(value) == 1;
        }();
        if (traceValidZero) {
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[SurfaceCache] tracing shadow for valid depth-zero texels" << std::endl;
        }
        auto shadowShader = CompileComputeShader(
            traceValidZero ? "SurfaceCacheShadow_validzero" : "SurfaceCacheShadow",
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
        if (i == 0 && static_composite_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{static_composite_set_layout_};
            static_composite_set_ = device_->CreateDescriptorSet(dsDesc);
        }
        if (i == 0 && page_clear_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{page_clear_set_layout_};
            page_clear_set_ = device_->CreateDescriptorSet(dsDesc);
        }
        if (isVk && capture_gather_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{capture_gather_set_layout_};
            capture_gather_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (isVk && card_fill_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{card_fill_set_layout_};
            card_fill_set_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (isVk && source_writer_audit_requested_ && !source_writer_audit_failed_ &&
            card_fill_writer_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{card_fill_writer_set_layout_};
            card_fill_writer_set_[i] = device_->CreateDescriptorSet(dsDesc);
            if (card_fill_writer_set_[i] == handles::INVALID_DESCRIPTOR_SET)
                source_writer_audit_failed_ = true;
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

    if (source_writer_audit_requested_ && !source_writer_audit_failed_) {
        source_writer_audit_enabled_ =
            source_writer_atlas_ != handles::INVALID_RESOURCE &&
            card_fill_writer_pipeline_ != handles::INVALID_PIPELINE &&
            card_fill_writer_layout_ != handles::INVALID_PIPELINE_LAYOUT;
        if (!source_writer_audit_enabled_) {
            source_writer_audit_failed_ = true;
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[SC_CAPTURE_SOURCE_AUDIT] writer resources incomplete; audit closed"
                << std::endl;
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

    if (source_writer_audit_requested_) {
        if (source_writer_audit_failed_ ||
            source_writer_atlas_ == handles::INVALID_RESOURCE ||
            device_->GetPlatform() != rhi::RHIPlatform::Vulkan) {
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[SC_CAPTURE_SOURCE_AUDIT] card capture writer unavailable; "
                << "diagnostic pass closed" << std::endl;
            source_writer_audit_enabled_ = false;
            return;
        }
    }
    if (geometry_audit_requested_ &&
        (geometry_audit_failed_ || !geometry_audit_enabled_)) {
        source_writer_audit_failed_ = true;
        source_writer_audit_enabled_ = false;
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[SC_CAPTURE_GEOMETRY_AUDIT] geometry witness resources unavailable; "
            << "diagnostic pass closed" << std::endl;
        return;
    }

    // [WRC 计划 Stream D1]（PRIMAL_SC_PRIORITY=1）：卡片重要性打分 + Top-K
    // 替换轮转游标。静态局部定义（文件内 AddPass 侧同读）。
    static const bool kScPriorityCapture = [] {
        const char* e = std::getenv("PRIMAL_SC_PRIORITY");
        return e && e[0] == '1';
    }();

    // [SC 卡片 Lumen 对齐 Phase 3 实验项 → SC per-mesh 计划 P2 生产约束]
    // （PRIMAL_SC_CAPTURE_OWNER，默认开；=0 回滚旧全场景写入路径）：
    // capture 只提交 owning 实例的 cluster。计划 §3.2"owner-only 不能仅作为
    // 诊断开关；最终生产路径必须建立不可绕过的约束"——静态卡已由离线页
    // 供给（合成后跳过 capture），本约束管住剩余 fallback 卡的运行时捕获；
    // 外来几何的 albedo/normal/depth 从此无法进入任何卡片。
    static const bool kScCaptureOwnerOnly = [] {
        const char* e = std::getenv("PRIMAL_SC_CAPTURE_OWNER");
        return !e || e[0] != '0';   // 默认 ON；"0" 显式回滚
    }();

    auto& gpuDraw = nanite::GPUDrivenDrawPipeline::Get();
    if (!gpuDraw.InitializeCardCaptureResources(1u << 20,
                                                source_writer_audit_requested_,
                                                geometry_audit_requested_)) {
        if (source_writer_audit_requested_) {
            source_writer_audit_failed_ = true;
            source_writer_audit_enabled_ = false;
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[SC_CAPTURE_SOURCE_AUDIT] GPU card writer pipeline unavailable; "
                << "no production fallback" << std::endl;
        }
        return;
    }
    if (source_writer_audit_requested_ && !gpuDraw.IsCardCaptureWriterReady()) {
        source_writer_audit_failed_ = true;
        source_writer_audit_enabled_ = false;
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[SC_CAPTURE_SOURCE_AUDIT] GPU card writer readiness check failed" << std::endl;
        return;
    }
    if (geometry_audit_requested_ && !gpuDraw.IsCardCaptureGeometryReady()) {
        geometry_audit_failed_ = true;
        source_writer_audit_failed_ = true;
        source_writer_audit_enabled_ = false;
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[SC_CAPTURE_GEOMETRY_AUDIT] geometry witness pipeline unavailable; "
            << "diagnostic closed without fallback" << std::endl;
        return;
    }
    if (source_writer_audit_requested_) {
        source_writer_audit_enabled_ = true;
        static bool writerReadyLogged = false;
        if (!writerReadyLogged) {
            writerReadyLogged = true;
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[SC_CAPTURE_SOURCE_AUDIT] card capture + CardFill writer pipelines ready; "
                << "RGBA32_UInt writer atlas bound" << std::endl;
        }
    }
    if (geometry_audit_requested_) {
        static bool geometryReadyLogged = false;
        if (!geometryReadyLogged) {
            geometryReadyLogged = true;
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[SC_CAPTURE_GEOMETRY_AUDIT] 8-MRT geometry witness pipeline ready; "
                << "four RGBA32_UInt targets bound" << std::endl;
        }
    }

    const auto& cards = card_generator_.GetCards();
    const u32 total = card_generator_.GetCardCount();
    if (total == 0) return;

    const bool qualified = device_->GetPlatform() == RHIPlatform::Vulkan && !LegacySurfacePolicy();
    RGResourceHandle materialDependency{};
    if (qualified) {
        materialDependency = graph.ImportResource("SC_SurfaceValidity", albedo_atlas_);
        struct PrepareData {};
        graph.AddPass<PrepareData>("SurfaceCachePrepare", RGPassType::Compute,
            RGPassCategory::Lighting,
            [materialDependency](PrepareData&, RenderGraphBuilder& builder) {
                builder.Write(materialDependency, ResourceState::UnorderedAccess);
                builder.SideEffect();
            },
            [this, current_frame_index](const PrepareData&, RenderGraphContext& ctx) {
                ResetSurfaceAtlas(ctx.cmdBuffer, DDGISlotsForFrame(current_frame_index).writeIndex);
            });
    }

    struct CapPassData {};
    graph.AddPass<CapPassData>(
        "SurfaceCacheCardCapture",
        RGPassType::Graphics,
        RGPassCategory::Lighting,
        [materialDependency](CapPassData&, RenderGraphBuilder& builder) {
            if (materialDependency.IsValid()) builder.Write(materialDependency, ResourceState::RenderTarget);
            builder.SideEffect();  // atlases are manually barriered module RTs
        },
        [this, &scene_snapshot, &gpuDraw, total, current_frame_index, qualified](const CapPassData&, RenderGraphContext& ctx) {
            auto* cmd = ctx.cmdBuffer;
            const auto& cards = card_generator_.GetCards();

            // Seed batch must stay ≤ kCardCaptureSlots (32): scratch slots are
            // CPU-uploaded at encode time, so folding >32 faces per frame onto
            // 32 slots overwrites data for draws earlier in the same buffer.
            u32 batch = card_view_seeded_ ? 8u : 32u;
            if (const char* b = std::getenv("SC_CARDVIEW_BATCH"))
                batch = (u32)std::max(1, atoi(b));

            // Storage (GENERAL) → color-attachment layouts.  Diagnostic
            // attachments start in ShaderResource after their blocking zero
            // upload, then remain in UnorderedAccess between frames.
            ResourceHandle pages[8] = {
                albedo_atlas_, normal_atlas_, depth_atlas_,
                source_writer_audit_enabled_ ? source_writer_atlas_ : handles::INVALID_RESOURCE,
                geometry_audit_enabled_ ? geometry_witness_[0] : handles::INVALID_RESOURCE,
                geometry_audit_enabled_ ? geometry_witness_[1] : handles::INVALID_RESOURCE,
                geometry_audit_enabled_ ? geometry_witness_[2] : handles::INVALID_RESOURCE,
                geometry_audit_enabled_ ? geometry_witness_[3] : handles::INVALID_RESOURCE
            };
            const u32 pageCount = geometry_audit_enabled_ ? 8u :
                (source_writer_audit_enabled_ ? 4u : 3u);
            for (u32 page = 0; page < pageCount; ++page) {
                const auto h = pages[page];
                ResourceBarrier b{};
                b.resource = h;
                const bool diagnostic = page >= 3u;
                const bool inUav = page == 3u ? source_writer_atlas_in_uav_
                                              : geometry_witness_in_uav_;
                b.beforeState = diagnostic && !inUav
                    ? ResourceState::ShaderResource : ResourceState::UnorderedAccess;
                b.afterState  = ResourceState::RenderTarget;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }

            u32 faces = 0;
            // 单卡片正交捕获（两条调度路径共享；faces = face-slot，帧内唯一）。
            auto captureOne = [&](const SurfaceCacheCard& card, u32 faceSlot) -> bool {
                // [SC per-mesh P2] 静态卡不走运行时 capture：表面数据只能来自
                // 离线 owner 页（§3.2 生产约束，非诊断开关——合成管线不可用的
                // 后端不设旗标，走原路径）。
                if (card._pad1[1] & 1u) return false;
                if (source_writer_audit_requested_ && !source_writer_audit_enabled_)
                    return false;
                if (geometry_audit_requested_ && !geometry_audit_enabled_)
                    return false;
                // R4: 裸 uint 是 POSIX typedef，MSVC 无此名（C2065/C2086 级联源）→ u32。
                const u32 axis = card.axis_direction & 0xFFu;
                const u32 dir  = (card.axis_direction >> 8u) & 0xFFu;

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
                // [R2.1 2026-09-20] 捕获近平面参数化：PRIMAL_SC_CAPTURE_NEAR
                //（默认 0.05 = 历史校准值，逐位现行为）。near 只控制光栅化
                // 裁剪（存储的 axis-depth 与 near 解耦——机制排查见
                // Docs/2026-09-20-sc-capture-near-plane-mechanism.md）；
                // near<0.05 实验须同时开捕获端 min-depth 无效化（同文档 §4）。
                static const f32 kCaptureNear = [] {
                    const char* e = std::getenv("PRIMAL_SC_CAPTURE_NEAR");
                    if (!e) return 0.05f;
                    const f32 v = static_cast<f32>(std::atof(e));
                    return (v >= 0.0f && v < 1.0f) ? v : 0.05f;
                }();
                const f32 captureFar = 2.0f * halfAxis * rhi::card_surface_identity::CaptureFarScale + rhi::card_surface_identity::CaptureFarBias;
                const math::m4x4 proj = rhimath::CreateOrthographicMatrix(
                    mirror_u ? hu : -hu, mirror_u ? -hu : hu, -hv, hv,
                    kCaptureNear, captureFar);

                const bool captureOk = gpuDraw.ExecuteCardCapturePass(
                    cmd, scene_snapshot, view, proj,
                    math::v4{center.x, center.y, center.z, 1.0f},
                    math::v4{fwd.x, fwd.y, fwd.z, 0.0f},
                    kCaptureNear, captureFar,
                    faceSlot,       // face_slot — unique within this frame
                    current_frame_index,
                    card.atlas_offset_x, card.atlas_offset_y, card.resolution,
                    albedo_atlas_, normal_atlas_, depth_atlas_, card_view_depth_,
                    /*owning_instance_id=*/card.mesh_instance_id,
                    /*owner_only=*/(kScCaptureOwnerOnly || qualified),
                    /*writer_rt=*/source_writer_audit_enabled_ ? source_writer_atlas_ : handles::INVALID_RESOURCE,
                    /*geometry_rt0=*/geometry_audit_enabled_ ? geometry_witness_[0] : handles::INVALID_RESOURCE,
                    /*geometry_rt1=*/geometry_audit_enabled_ ? geometry_witness_[1] : handles::INVALID_RESOURCE,
                    /*geometry_rt2=*/geometry_audit_enabled_ ? geometry_witness_[2] : handles::INVALID_RESOURCE,
                    /*geometry_rt3=*/geometry_audit_enabled_ ? geometry_witness_[3] : handles::INVALID_RESOURCE);
                if (!captureOk && (source_writer_audit_requested_ || geometry_audit_requested_)) {
                    source_writer_audit_failed_ = true;
                    source_writer_audit_enabled_ = false;
                    geometry_audit_failed_ = geometry_audit_requested_;
                    geometry_audit_enabled_ = false;
                    PRIMAL_LOG_STREAM(Error, "Render")
                        << (geometry_audit_requested_ ? "[SC_CAPTURE_GEOMETRY_AUDIT] "
                                                       : "[SC_CAPTURE_SOURCE_AUDIT] ")
                        << "card writer draw preparation failed; "
                        << "diagnostic closed without production fallback" << std::endl;
                }
                return captureOk;
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
                    const u32 capAxis = c.axis_direction & 0xFFu;
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
            for (u32 page = 0; page < pageCount; ++page) {
                const auto h = pages[page];
                ResourceBarrier b{};
                b.resource = h;
                b.beforeState = ResourceState::RenderTarget;
                b.afterState  = ResourceState::UnorderedAccess;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            }
            if (source_writer_audit_enabled_ || geometry_audit_enabled_ || qualified) {
                // These images may be consumed by CardFill or a later transfer
                // read. State transitions alone do not carry the writer's
                // color-attachment visibility on every RHI.
                cmd->MemoryBarrier(PipelineStage::ColorAttachmentOutput,
                                   PipelineStage::ComputeShader,
                                   AccessFlag::ColorAttachmentWrite,
                                   AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
            }
            if (source_writer_audit_enabled_) source_writer_atlas_in_uav_ = true;
            if (geometry_audit_enabled_) geometry_witness_in_uav_ = true;
            if (lumencompare::enabled) {
                lumencompare::Output(device_, cmd, "SurfaceCache.Capture", "albedo", albedo_atlas_, false);
                lumencompare::Output(device_, cmd, "SurfaceCache.Capture", "depth", depth_atlas_, false);
                lumencompare::Output(device_, cmd, "SurfaceCache.Capture", "normal", normal_atlas_, false);
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
    const u32 axis = card.axis_direction & 0xFFu;
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
    // The opt-in Vulkan slot experiment requires explicit proof
    // that the renderer's write slot matches a successfully completed host
    // fence. A frame counter alone is not such proof. The default path keeps
    // its existing caller-owned fence contract (including its known timeout
    // boundary); neither path certifies other shared Surface Cache resources.
    if (experimental_light_dispatch_slots_) {
        const auto& completed = device_->FrameSlotCompletions();
        if (!completed.CanReuse(outIdx, light_dispatch_consumed_serial_[outIdx])) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] light/dispatch slot experiment rejected: slot not completed: " << outIdx;
            return output;
        }
        light_dispatch_consumed_serial_[outIdx] = completed.Serial(outIdx);
    }
    if (GetLightInfoBuffer(outIdx) == handles::INVALID_RESOURCE ||
        GetCardDispatchBuffer(outIdx) == handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[SurfaceCache] light/dispatch frame slot unavailable: " << outIdx;
        return output;
    }

    // Import persistent lighting atlas into render graph
    auto lightingHandle = graph.ImportResource(
        "SurfaceCache_Lighting_" + std::to_string(outIdx),
        lighting_atlas_[outIdx]);
    // [E0-B 2026-09-21] nodirect 副本同帧身份——Write(UA) 声明闭环布局轮转
    //（SC 写 UA → SPGI 读 SR），避免显式 barrier 与 RG 追踪错位。
    auto nodirectHandle = graph.ImportResource(
        "SurfaceCache_Lighting_NoDirect_" + std::to_string(outIdx),
        lighting_nodirect_atlas_[outIdx]);

    output.lighting_atlas = lightingHandle;
    output.lighting_nodirect_atlas = nodirectHandle;
    const bool qualifiedSurface = device_->GetPlatform() == RHIPlatform::Vulkan && !LegacySurfacePolicy();
    const auto materialDependency = qualifiedSurface
        ? graph.ImportResource("SC_SurfaceValidity", albedo_atlas_) : RGResourceHandle{};

    graph.AddPass<SurfaceCacheRGData>("LumenSurfaceCache",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [prev_frame_color, lightingHandle, nodirectHandle, frame_data, materialDependency](
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
            // [E0-B] nodirect 副本同帧 Write(UA)——LightEval 的第二个存储写。
            builder.Write(nodirectHandle, ResourceState::UnorderedAccess);
            if (materialDependency.IsValid()) builder.Write(materialDependency, ResourceState::UnorderedAccess);
        },

        // ====================================================================
        // Execute lambda: merged per-card texel lighting dispatch
        // ====================================================================
        [this, frame_data, current_frame_index, outIdx, lightingHandle](
            const SurfaceCacheRGData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            const u32 frameIdx = DDGISlotsForFrame(current_frame_index).writeIndex;
            if (device_->GetPlatform() == RHIPlatform::Vulkan && !LegacySurfacePolicy())
                ResetSurfaceAtlas(cmd, frameIdx);

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
                // [SC per-mesh P2] bit0 = static：CardFill 据此跳过（静态卡
                // 表面只能来自离线 owner 页，SDF 回填不得覆盖——§3.2）。
                dispatch_info[i]._pad[0] = cards[i]._pad1[1] & 1u;
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
                auto* mapped = static_cast<CardDispatchInfo*>(device_->MapBuffer(GetCardDispatchBuffer(frameIdx)));
                if (!mapped) return;
                memcpy(mapped, dispatch_info.data(), card_count * sizeof(CardDispatchInfo));
                device_->UnmapBuffer(GetCardDispatchBuffer(frameIdx));
            }

            // ================================================================
            // 2. Upload light list into GetLightInfoBuffer(frameIdx) (T3.2: direction/
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
                auto* lights = static_cast<LightInfoGPU*>(device_->MapBuffer(GetLightInfoBuffer(frameIdx)));
                if (!lights) return;
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
                    device_->UnmapBuffer(GetLightInfoBuffer(frameIdx));
                }
                last_light_info_slot_ = frameIdx;
            }
            // The experimental slot path publishes its mapped inputs before
            // GPU consumption. Default keeps the prior synchronization path.
            if (experimental_light_dispatch_slots_)
                cmd->MemoryBarrier(PipelineStage::Host, PipelineStage::ComputeShader,
                                   AccessFlag::HostWrite, AccessFlag::ShaderRead);

            // ================================================================
            // 3. Upload FlattenedLightingParams constant buffer
            // ================================================================
            {
                auto* mapped = static_cast<FlattenedLightingParams*>(device_->MapBuffer(params_cb_[frameIdx]));
                if (mapped) {
                    FlattenedLightingParams params{};
                    params.sc_params._pad = device_->GetPlatform() == RHIPlatform::Vulkan && LegacySurfacePolicy() ? 1u : 0u;
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
                    // WRC 计划 Stream B1: bootstrap 模式（默认开）下探针馈送
                    // 边彻底切断（E1 边消失——能量单向化的最终态；暖启动窗口
                    // 内 entire GI 回路冻结 = 确定性相位 1）。旧语义（bootstrap
                    // 关）保留窗口后常开馈送。
                    params.ddgi_counts_valid[3] =
                        (frame_data.ddgi_irradiance != rhi::handles::INVALID_RESOURCE &&
                         !std::getenv("SC_PROBE_FEED_OFF") &&
                         !kProbeFeedBootstrap &&
                         frame_data.gi_frame >= scWarmupFrames())
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
                    // 暖启动窗口内 tile 求解器冻结（确定性相位 1——atlas 纯
                    // 直射收敛，无任何回路运行）；窗口后统一启动。
                    params.radiosity_gate[0] =
                        scIndirectEnabled(device_) &&
                        frame_data.gi_frame >= scWarmupFrames() ? 1u : 0u;
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
                    params.radiosity_gate[1] = [this] {
                        // [2026-09-20 P5 默认 2.0→1.0] 物理衰减。w=2.0 的每跳
                        // 有效增益 w×ρ 达 0.6-1.0，稳态放大 2.5×（物理 1.43×），
                        // 阴影被人为泵住（差距分析 §2.5）；闩锁修复后重跑 A/B
                        // （tier0.25、frame780 锁机位）：w=1.0 室内 ROI 32.6→28.5
                        // （−13%）、lit/shadow 4.4→5.1×，直射区不变。
                        // [2026-09-25 Vulkan 参考机位] 1.0→0.5：第 350 帧
                        // 右廊 37.36→32.95、近地 93.25→87.40，亮墙与天空
                        // 仍接近参考。只调整已验证的 Vulkan 默认；Metal/Dawn
                        // 维持 1.0，跨端校准须另行 A/B。环境变量可逐次覆盖。
                        // 回退：PRIMAL_SC_RADIOSITY_W=1.0。
                        const char* e = std::getenv("PRIMAL_SC_RADIOSITY_W");
                        const float defaultWeight =
                            device_->GetPlatform() == rhi::RHIPlatform::Vulkan ? 0.5f : 1.0f;
                        const float v = e ? (float)std::atof(e) : defaultWeight;
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
                        lumen::GIProbeBootstrapFactor(frame_data.gi_frame);
                    params.radiosity_solo = kProbeFeedBootstrap ? 1u : 0u;
                    // [WRC 计划 Stream D2]: LightEval 采样契约过表寻址开关。
                    params.page_table_enabled =
                        card_generator_.IsPageTableEnabled() ? 1u : 0u;
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
            if (source_writer_audit_enabled_ && !source_writer_atlas_in_uav_) {
                // No card-view draw may have run on an empty/late card list;
                // the off-frame zero upload leaves the persistent writer in
                // ShaderResource layout.  Bring it to the storage layout before
                // the diagnostic CardFill writer can bind it.
                ResourceBarrier writerInit{};
                writerInit.resource = source_writer_atlas_;
                writerInit.beforeState = ResourceState::ShaderResource;
                writerInit.afterState = ResourceState::UnorderedAccess;
                writerInit.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&writerInit, 1);
                source_writer_atlas_in_uav_ = true;
            }
            if (geometry_audit_enabled_ && !geometry_witness_in_uav_) {
                ResourceBarrier witnessInit[4]{};
                for (u32 i = 0; i < 4; ++i) {
                    witnessInit[i].resource = geometry_witness_[i];
                    witnessInit[i].beforeState = ResourceState::ShaderResource;
                    witnessInit[i].afterState = ResourceState::UnorderedAccess;
                    witnessInit[i].subresource = 0xFFFFFFFF;
                }
                cmd->InsertBarrier(witnessInit, 4);
                geometry_witness_in_uav_ = true;
            }

            // ================================================================
            // 3.5 AtlasInit dispatch (Vulkan-only, FIRST FRAME ONLY): write
            //     default material into card atlas regions so LightEval has
            //     alpha=1 albedo to process even before Capture covers texels.
            // ================================================================
            if (atlas_init_pipeline_ != handles::INVALID_PIPELINE &&
                (device_->GetPlatform() != RHIPlatform::Vulkan || LegacySurfacePolicy()) &&
                (!atlas_seeded_ || surface_seed_epoch_ != card_generator_.GetAllocationEpoch() ||
                 (StaticPagesEnabled() && static_composite_epoch_ != card_generator_.GetAllocationEpoch()))) {
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
                    {aiIsVk ? 5u : 2u, DescriptorType::StorageBuffer, GetCardDispatchBuffer(frameIdx)},
                    {6, DescriptorType::StorageBuffer, triangle_atlas_buffer_},
                };
                UpdateDescriptorSet(device_, atlas_init_set_[frameIdx], initParams, aiIsVk?7u:6u);

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
                if (total_texels > 0) {
                    atlas_seeded_ = true;
                    surface_seed_epoch_ = card_generator_.GetAllocationEpoch();
                    RequestTileValidityReset();
                }

                // Compute→compute memory barrier (storage images stay in
                // GENERAL layout; LightEval reads via imageLoad).
                cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                   AccessFlag::ShaderWrite, AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
            }

            // ================================================================
            // 3.55 [SC per-mesh 计划 P2] 静态页合成（Vulkan-only，分配纪元
            //      变化时一次）：owner mesh 离线页 → SC albedo/normal/depth
            //      atlas。合成后静态卡跳过 capture/CardFill（§3.2 外来写入
            //      生产约束）；单卡页缺失 → 该卡保持显式无效种子并记日志
            //      （缺页可区分，不静默伪有效）。
            // ================================================================
            if (static_composite_pipeline_ != handles::INVALID_PIPELINE &&
                static_composite_set_ != handles::INVALID_DESCRIPTOR_SET &&
                StaticPagesEnabled() &&
                card_generator_.GetAllocationEpoch() != static_composite_epoch_) {
                const auto& scCards = card_generator_.GetCards();
                const auto& scRefs = card_generator_.GetStaticRefs();
                std::vector<StaticCompositeInfo> scInfos;
                std::vector<u32> scPool;
                boundary_atlas_groups_.clear();boundary_atlas_epoch_=UINT32_MAX;
                bool boundaryAtlasValid=true;
                const char* boundaryAutoIndex=std::getenv("SC_BOUNDARY_AUTO_INDEX");
                const bool collectBoundaryAtlas=boundary_continuous_requested_ || (boundaryAutoIndex && std::strcmp(boundaryAutoIndex,"1")==0);
                const char* presenceEnv=std::getenv("PRIMAL_SC_CARD_TRIANGLE_FILTER");
                const char* maskEnv=std::getenv("PRIMAL_SC_OWNER_TRIANGLE_MASK");
                const bool buildMask=maskEnv && (std::strcmp(maskEnv,"0")==0 || std::strcmp(maskEnv,"1")==0);
                const bool buildPresence=buildMask || (presenceEnv && (std::strcmp(presenceEnv,"0")==0 || std::strcmp(presenceEnv,"1")==0));
                TriangleSupportIndex support(u32(scCards.size()),card_generator_.GetAllocationEpoch(),buildPresence);
                triangle_support_epoch_=0xffffffffu;
                u32 scTexelOffset = 0;
                u32 scMissing = 0;
                bool compositeUploaded = false;
                const char* resampleEnv = std::getenv("PRIMAL_SC_GEOMETRY_RESAMPLE");
                const bool geometryResample = geometry_first_requested_ && (!resampleEnv || std::strcmp(resampleEnv,"1")==0);
                const auto resampleStart = std::chrono::steady_clock::now();
                u64 resampleValid=0,resampleNoSource=0,resampleMiss=0;
                u32 resampledCards=0;
                for (size_t i = 0; i < scCards.size(); ++i) {
                    const SurfaceCacheCard& c = scCards[i];
                    const CardGenerator::CardStaticRef& ref = i < scRefs.size()
                        ? scRefs[i] : CardGenerator::CardStaticRef{};
                    if ((c._pad1[1] & 1u) == 0u ||
                        ref.mesh_slot == CardGenerator::kNoStaticSlot) continue;
                    std::vector<u8> pageAlbedo, pageNormal;
                    std::vector<f32> pageDepth;
                    std::vector<u32> pageTriangles;
                    u32 pageRes = 0;
                    if (!static_pages_->ReadCardPlanes(ref.mesh_slot, c._pad1[2],
                                                       pageAlbedo, pageNormal, pageDepth, pageRes, &pageTriangles)) {
                        ++scMissing;
                        continue;   // 缺页卡：保持显式无效种子（black + 日志可辨）
                    }
                    const u32 res = c.resolution;
                    if (geometryResample && pageRes != res) {
                        const auto instance = std::find_if(geometry_scene_.instances.begin(),geometry_scene_.instances.end(),
                            [&](const auto& value){return value.instance_id==c.mesh_instance_id;});
                        GeometrySurfacePage rebuilt;
                        const float center[3]={ref.local_center.x,ref.local_center.y,ref.local_center.z};
                        const float extent[3]={ref.local_extent.x,ref.local_extent.y,ref.local_extent.z};
                        if (instance==geometry_scene_.instances.end() || !ResampleGeometrySurface(
                            geometry_scene_.bottom,geometry_scene_.triangles,instance->root,center,extent,
                            ref.local_axis_direction,pageRes,res,pageAlbedo,pageNormal,pageTriangles,rebuilt)) {
                            ++scMissing;continue; // Unavailable geometry cannot certify a resampled surface.
                        }
                        resampleValid+=rebuilt.valid;resampleNoSource+=rebuilt.no_source;resampleMiss+=rebuilt.geometry_miss;
                        ++resampledCards;
                        pageAlbedo=std::move(rebuilt.albedo);pageNormal=std::move(rebuilt.normal);
                        pageDepth=std::move(rebuilt.depth);pageTriangles=std::move(rebuilt.triangles);pageRes=res;
                    }
                    // Only positive diagonal transforms preserve source pixel ordering.
                    // Every indexed pixel is also revalidated against the actual GPU atlas.
                    const auto wm0=math::column(ref.world,0),wm1=math::column(ref.world,1),wm2=math::column(ref.world,2);
                    const bool aligned=wm0.x>0 && wm1.y>0 && wm2.z>0 &&
                        wm0.y==0 && wm0.z==0 && wm1.x==0 && wm1.z==0 && wm2.x==0 && wm2.y==0;
                    if(geometry_first_requested_ && aligned && pageRes==res)
                        support.Add(u32(i),res,pageTriangles,pageDepth,pageAlbedo);
                    if(collectBoundaryAtlas && BoundarySamplesForStaticSlot(ref.mesh_slot) && aligned && pageRes==res){
                        if(!AppendBoundaryAtlasPoints(c,ref.world,u32(i),ref.local_axis_direction,pageRes,
                            pageTriangles,pageDepth,pageAlbedo,boundary_atlas_groups_))boundaryAtlasValid=false;
                    }
                    StaticCompositeInfo info{};
                    info.meta0[0] = c.atlas_offset_x;
                    info.meta0[1] = c.atlas_offset_y;
                    info.meta0[2] = res;
                    info.meta0[3] = pageRes;
                    info.meta1[0] = scTexelOffset;
                    info.meta1[1] = 0u;   // 首条稍后写 static_count
                    info.meta1[2] = c.axis_direction;
                    info.meta1[3] = ref.local_axis_direction;
                    info.meta2[0] = (u32)scPool.size();
                    scPool.insert(scPool.end(), pageAlbedo.size() / 4u, 0u);
                    memcpy(scPool.data() + info.meta2[0], pageAlbedo.data(), pageAlbedo.size());
                    info.meta2[1] = (u32)scPool.size();
                    scPool.insert(scPool.end(), pageNormal.size() / 4u, 0u);
                    memcpy(scPool.data() + info.meta2[1], pageNormal.data(), pageNormal.size());
                    info.meta2[2] = AppendStaticDepthPlane(scPool, pageDepth);
                    info.meta2[3] = (u32)scPool.size();
                    scPool.insert(scPool.end(),pageTriangles.begin(),pageTriangles.end());
                    info.center_w[0] = c.center.x; info.center_w[1] = c.center.y;
                    info.center_w[2] = c.center.z; info.center_w[3] = 0.0f;
                    for (u32 k = 0; k < 3; ++k) {
                        const math::v4 col = math::column(ref.world, k);
                        float* dst = (k == 0) ? info.mcol0 : (k == 1) ? info.mcol1 : info.mcol2;
                        dst[0] = col.x; dst[1] = col.y; dst[2] = col.z; dst[3] = 0.0f;
                    }
                    math::v4 emission{};
                    if (const char* e = std::getenv("PRIMAL_SC_EMISSIVE")) {
                        float r=0, g=0, b=0;
                        if (std::sscanf(e, "%f,%f,%f", &r, &g, &b) == 3) emission = math::v4{r,g,b,0.f};
                    }
                    if (c.mesh_instance_id < kMaxEmissiveInstances && instance_emissive_cpu_[c.mesh_instance_id].w > .5f)
                        emission = instance_emissive_cpu_[c.mesh_instance_id];
                    info.local0[0] = emission.x;
                    info.local0[1] = emission.y;
                    info.local0[2] = emission.z;
                    info.local0[3] = ref.local_extent.x;
                    info.local1[0] = ref.local_extent.y;
                    info.local1[1] = ref.local_extent.z;
                    scInfos.push_back(info);
                    scTexelOffset += res * res;
                }
                if(geometryResample) PRIMAL_LOG_STREAM(Info,"Render") << "[SC_GEOMETRY_RESAMPLE] cards=" << resampledCards
                    << " valid=" << resampleValid << " no_source=" << resampleNoSource << " geometry_miss=" << resampleMiss
                    << " missing=" << scMissing << " milliseconds=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now()-resampleStart).count();
                if (!scInfos.empty() && scTexelOffset > 0) {
                    scInfos[0].meta1[1] = (u32)scInfos.size();   // shader 侧 count 槽
                    // 上传（销毁重建——epoch 级一次性）。
                    if (static_composite_info_buffer_ != handles::INVALID_RESOURCE)
                        device_->DestroyBuffer(static_composite_info_buffer_);
                    if (static_composite_pool_buffer_ != handles::INVALID_RESOURCE)
                        device_->DestroyBuffer(static_composite_pool_buffer_);
                    auto makeBuf = [&](rhi::ResourceHandle& h, const void* data, u64 bytes) {
                        rhi::BufferDesc desc{};
                        desc.size = bytes;
                        desc.type = rhi::BufferType::Structured;
                        desc.usage = rhi::GPUMemoryUsage::Dynamic;
                        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                        desc.structured.elementCount = (u32)((bytes + 3u) / 4u);
                        desc.structured.elementStride = 4;   // 按 uint word 寻址（shader 侧 std430 uint[]）
                        h = device_->CreateBuffer(desc);
                        if (h != handles::INVALID_RESOURCE) {
                            void* mapped = device_->MapBuffer(h);
                            if (mapped) { memcpy(mapped, data, bytes); device_->UnmapBuffer(h); }
                            else { device_->DestroyBuffer(h); h = handles::INVALID_RESOURCE; }
                        }
                    };
                    if(geometry_first_requested_) {
                        if(triangle_support_buffer_!=handles::INVALID_RESOURCE) device_->DestroyBuffer(triangle_support_buffer_);
                        if(buildMask){std::vector<u32> owners;owners.reserve(scCards.size());for(const auto& card:scCards)owners.push_back(card.mesh_instance_id);
                            support.FinalizeOwnerMasks(owners,card_generator_.GetLookupCount());}
                        if(buildPresence)support.FinalizePresence();
                        makeBuf(triangle_support_buffer_,support.words.data(),u64(support.words.size())*sizeof(u32));
                        triangle_support_word_count_=triangle_support_buffer_!=handles::INVALID_RESOURCE?u32(support.words.size()):0u;
                        if(triangle_support_buffer_!=handles::INVALID_RESOURCE) triangle_support_epoch_=card_generator_.GetAllocationEpoch();
                        PRIMAL_LOG_STREAM(Info,"Render") << "[SC_TRIANGLE_SUPPORT] words=" << support.words.size()
                            << " epoch=" << triangle_support_epoch_;
                    }
                    makeBuf(static_composite_info_buffer_, scInfos.data(),
                            u64(scInfos.size()) * sizeof(StaticCompositeInfo));
                    makeBuf(static_composite_pool_buffer_, scPool.data(),
                            u64(scPool.size()) * sizeof(u32));
                    if (static_composite_info_buffer_ != handles::INVALID_RESOURCE &&
                        static_composite_pool_buffer_ != handles::INVALID_RESOURCE) {
                        const bool ccIsVk = device_->GetPlatform() == rhi::RHIPlatform::Vulkan;
                        DescriptorData ccParams[] = {
                            {0, DescriptorType::StorageImage,  albedo_atlas_},
                            {1, DescriptorType::StorageImage,  normal_atlas_},
                            {2, DescriptorType::StorageImage,  depth_atlas_},
                            {3, DescriptorType::StorageBuffer, static_composite_info_buffer_},
                            {4, DescriptorType::StorageBuffer, static_composite_pool_buffer_},
                            {5, DescriptorType::StorageImage, emissive_atlas_},
                            {6, DescriptorType::StorageBuffer, triangle_atlas_buffer_},
                        };
                        (void)ccIsVk;
                        UpdateDescriptorSet(device_, static_composite_set_, ccParams, 7);
                        cmd->MemoryBarrier(PipelineStage::Host, PipelineStage::ComputeShader,
                                           AccessFlag::HostWrite, AccessFlag::ShaderRead);
                        cmd->BindComputePipeline(static_composite_pipeline_);
                        const DescriptorSetHandle ccSets[] = { static_composite_set_ };
                        cmd->BindDescriptorSets(PipelineBindPoint::Compute, static_composite_layout_,
                                                0, 1, ccSets, 0, nullptr);
                        const u32 ccGroups = (scTexelOffset + 255) / 256;
                        cmd->Dispatch(ccGroups, 1, 1);
                        cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                           AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
                        compositeUploaded = true;
                        static_composite_card_count_ = (u32)scInfos.size();
                        static_composite_pool_bytes_ = u64(scPool.size()) * 4u;
                        PRIMAL_LOG_STREAM(Error, "Render")
                            << "[SC_STATIC] composite dispatched: cards=" << scInfos.size()
                            << " texels=" << scTexelOffset
                            << " poolMB=" << (static_composite_pool_bytes_ >> 20)
                            << " missing_pages=" << scMissing << std::endl;
                    }
                } else if (scMissing > 0) {
                    PRIMAL_LOG_STREAM(Error, "Render")
                        << "[SC_STATIC] no composite dispatched, missing_pages=" << scMissing << std::endl;
                }
                if (compositeUploaded && scMissing == 0)
                    static_composite_epoch_ = card_generator_.GetAllocationEpoch();
                    if(collectBoundaryAtlas && boundaryAtlasValid)boundary_atlas_epoch_=static_composite_epoch_;
            }

            // ================================================================
            // 3.56 [SC per-mesh 计划 P2] 页复用拒绝旧引用（Vulkan-only）：
            //      页表模式 LRU 挤页释放的 atlas rect → 6 个 lighting atlas
            //      清零。旧引用在物理页复用前被显式拒绝（§3.1）。
            // ================================================================
            if (page_clear_pipeline_ != handles::INVALID_PIPELINE &&
                page_clear_set_ != handles::INVALID_DESCRIPTOR_SET &&
                card_generator_.GetAllocationEpoch() != page_clear_epoch_) {
                // Whole allocation was repacked: reject every previous identity, not only LRU slices.
                const std::vector<CardGenerator::EvictedRect> evicted = {{0, 0, atlas_size_, atlas_size_}};
                if (!evicted.empty()) {
                    std::vector<u32> rectWords;
                    rectWords.push_back((u32)evicted.size());
                    u32 clearTexels = 0;
                    for (const auto& r : evicted) {
                        rectWords.push_back(r.x); rectWords.push_back(r.y);
                        rectWords.push_back(r.w); rectWords.push_back(r.h);
                        clearTexels += r.w * r.h;
                    }
                    if (page_clear_rects_buffer_ != handles::INVALID_RESOURCE)
                        device_->DestroyBuffer(page_clear_rects_buffer_);
                    rhi::BufferDesc rd{};
                    rd.size = u64(rectWords.size()) * sizeof(u32);
                    rd.type = rhi::BufferType::Structured;
                    rd.usage = rhi::GPUMemoryUsage::Dynamic;
                    rd.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                    rd.structured.elementCount = (u32)rectWords.size();
                    rd.structured.elementStride = sizeof(u32);
                    page_clear_rects_buffer_ = device_->CreateBuffer(rd);
                    if (page_clear_rects_buffer_ != handles::INVALID_RESOURCE) {
                        if (auto* mp = static_cast<u32*>(device_->MapBuffer(page_clear_rects_buffer_))) {
                            memcpy(mp, rectWords.data(), rd.size);
                            device_->UnmapBuffer(page_clear_rects_buffer_);
                        }
                        DescriptorData pcParams[] = {
                            {0, DescriptorType::StorageImage,  lighting_atlas_[0]},
                            {1, DescriptorType::StorageImage,  lighting_atlas_[1]},
                            {2, DescriptorType::StorageImage,  lighting_atlas_[2]},
                            {3, DescriptorType::StorageImage,  lighting_nodirect_atlas_[0]},
                            {4, DescriptorType::StorageImage,  lighting_nodirect_atlas_[1]},
                            {5, DescriptorType::StorageImage,  lighting_nodirect_atlas_[2]},
                            {6, DescriptorType::StorageBuffer, page_clear_rects_buffer_},
                        };
                        rhi::ResourceBarrier clearBarriers[6]{};
                        for (u32 k = 0; k < 6u; ++k) {
                            clearBarriers[k].resource = pcParams[k].resource;
                            clearBarriers[k].beforeState = rhi::ResourceState::Unknown;
                            clearBarriers[k].afterState = rhi::ResourceState::UnorderedAccess;
                            clearBarriers[k].subresource = 0xFFFFFFFFu;
                        }
                        cmd->InsertBarrier(clearBarriers, 6);
                        cmd->MemoryBarrier(PipelineStage::Host, PipelineStage::ComputeShader,
                                           AccessFlag::HostWrite, AccessFlag::ShaderRead);
                        UpdateDescriptorSet(device_, page_clear_set_, pcParams, 7);
                        cmd->BindComputePipeline(page_clear_pipeline_);
                        const DescriptorSetHandle pcSets[] = { page_clear_set_ };
                        cmd->BindDescriptorSets(PipelineBindPoint::Compute, page_clear_layout_,
                                                0, 1, pcSets, 0, nullptr);
                        const u32 pcGroups = (clearTexels + 255) / 256;
                        if (pcGroups > 0) cmd->Dispatch(pcGroups, 1, 1);
                        cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                           AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
                    }
                }
                page_clear_epoch_ = card_generator_.GetAllocationEpoch();
            }

            // ================================================================
            // 3.6 Capture dispatch (Vulkan-only, EVERY FRAME): GBuffer-gather —
            //     scatter real rendered surfaces (albedo / world normal / axis
            //     depth) into card atlas texels. Uncovered texels keep previous
            //     content (persistent atlas = cache semantics).
            // ================================================================
            if (!card_view_enabled_ && !source_writer_audit_requested_ &&
                GIDebug().sc_capture &&   // [F3-V4] 死旋钮接门控（false=跳过 dispatch，atlas 保持上帧内容）
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
            if (source_writer_audit_requested_ && !source_writer_audit_enabled_) {
                // The opt-in audit is fail-closed.  In particular, do not
                // dispatch the production CardFill shader after a missing
                // writer variant and later call its atlas state evidence.
                static bool writerFailureLogged = false;
                if (!writerFailureLogged) {
                    writerFailureLogged = true;
                    PRIMAL_LOG_STREAM(Error, "Render")
                        << "[SC_CAPTURE_SOURCE_AUDIT] CardFill writer unavailable; "
                        << "production CardFill dispatch suppressed" << std::endl;
                }
            } else if (GIDebug().sc_capture &&   // [F3-V4] 死旋钮接门控
                       (source_writer_audit_enabled_
                            ? card_fill_writer_pipeline_ != handles::INVALID_PIPELINE
                            : card_fill_pipeline_ != handles::INVALID_PIPELINE)) {
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

                // Single-variable, default-off CardFill source control. Bind
                // matching coordinates/resolution with the texture; do not
                // silently substitute coarse runtime data if unavailable.
                const char* fillSource = std::getenv("SC_FILL_OFFLINE_SDF");
                const bool offlineFillRequested = device_->GetPlatform() == RHIPlatform::Vulkan &&
                    (!LegacySurfacePolicy() ? HasOfflineSDFSource() : (fillSource && fillSource[0] == '1'));
                const bool offlineFill = offlineFillRequested &&
                    sc_offline_sdf_texture_ != handles::INVALID_RESOURCE &&
                    sc_offline_sdf_resolution_ > 0u;
                if (offlineFillRequested) {
                    cascadeCount = offlineFill ? 1u : 0u;
                    if (offlineFill) {
                        for (auto& tex : sdfTex) tex = sc_offline_sdf_texture_;
                    } else if (current_frame_index == 0u) {
                        PRIMAL_LOG_STREAM(Error, "Render") << "[SC_FILL_SOURCE] offline requested but unavailable; fill skipped" << std::endl;
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
                        static_assert(sizeof(CardFillParamsCB) == 160);
                        auto* cfm = static_cast<CardFillParamsCB*>(device_->MapBuffer(card_fill_cb_[frameIdx]));
                        if (cfm) {
                            for (u32 c = 0; c < cascadeCount; ++c) {
                                const auto& cascade = globalSDF.GetCascade(c);
                                cfm->sdf_origins[c] = {cascade.origin.x, cascade.origin.y, cascade.origin.z, 0.0f};
                                cfm->sdf_extents[c] = {cascade.extent.x, cascade.extent.y, cascade.extent.z, 0.0f};
                                cfm->sdf_res[c] = cascade.resolution;
                            }
                            if (offlineFill) {
                                cfm->sdf_origins[0] = {sc_offline_sdf_origin_.x, sc_offline_sdf_origin_.y, sc_offline_sdf_origin_.z, 0.0f};
                                cfm->sdf_extents[0] = {sc_offline_sdf_extent_.x, sc_offline_sdf_extent_.y, sc_offline_sdf_extent_.z, 0.0f};
                                cfm->sdf_res[0] = sc_offline_sdf_resolution_;
                            }
                            cfm->sdf_count = cascadeCount;
                            cfm->light_direction = math::v4{0.707f, -1.0f, 0.408f, 0.0f};
                            cfm->light_color = math::v4{5.0f, 5.0f, 5.0f, 1.0f};
                            cfm->total_texels = total_texels;
                            cfm->card_count = card_count;
                            cfm->_pad0 = cfm->_pad1 = 0;
                            // Diagnostic-only flags in existing unused words;
                            // CardFill is Vulkan-only. Metal/Dawn stay zero.
                            if (device_->GetPlatform() == RHIPlatform::Vulkan) {
                                const char* originHit = std::getenv("SC_FILL_REJECT_ORIGIN_HIT");
                                const char* axisLimit = std::getenv("SC_FILL_AXIS_LIMIT");
                                const char* materialBits = std::getenv("SC_FILL_MATERIAL_BITS");
                                cfm->_pad0 = originHit && originHit[0] == '1' ? 1u : 0u;
                                cfm->_pad1 = axisLimit && axisLimit[0] == '1' ? 1u : 0u;
                                if(materialBits && materialBits[0]=='1') cfm->_pad0 |= 2u;
                                if (LegacySurfacePolicy()) cfm->_pad0 |= 0x80000000u;
                            }
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
                        {8, DescriptorType::StorageBuffer, GetCardDispatchBuffer(frameIdx)},
                        {9, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()},
                        {10, DescriptorType::StorageBuffer, instBuf},
                        {11, DescriptorType::StorageBuffer, matBuf},
                        {12, DescriptorType::SampledImage,  albedoTexArr},
                        {13, DescriptorType::Sampler,       static_cast<ResourceHandle>(s_cf_sampler)},
                        {16, DescriptorType::StorageImage,  emissive_atlas_},
                    };
                    DescriptorData writerParams[17];
                    if (source_writer_audit_enabled_) {
                        std::memcpy(writerParams, fillParams, sizeof(fillParams));
                        writerParams[15] = {14, DescriptorType::StorageImage, source_writer_atlas_};
                        writerParams[16] = {15, DescriptorType::UniformBuffer, card_fill_writer_cb_[frameIdx]};
                        struct WriterParams { u32 frame, pad0, pad1, pad2; };
                        auto* writerCB = static_cast<WriterParams*>(device_->MapBuffer(card_fill_writer_cb_[frameIdx]));
                        if (!writerCB) {
                            source_writer_audit_failed_ = true;
                            source_writer_audit_enabled_ = false;
                            PRIMAL_LOG_STREAM(Error, "Render")
                                << "[SC_CAPTURE_SOURCE_AUDIT] writer frame CB map failed" << std::endl;
                            return;
                        }
                        writerCB->frame = current_frame_index;
                        writerCB->pad0 = writerCB->pad1 = writerCB->pad2 = 0;
                        device_->UnmapBuffer(card_fill_writer_cb_[frameIdx]);
                        UpdateDescriptorSet(device_, card_fill_writer_set_[frameIdx], writerParams, 17);
                    } else {
                        UpdateDescriptorSet(device_, card_fill_set_[frameIdx], fillParams, 15);
                    }

                    cmd->BindComputePipeline(source_writer_audit_enabled_
                        ? card_fill_writer_pipeline_ : card_fill_pipeline_);
                    const DescriptorSetHandle fillSets[] = { source_writer_audit_enabled_
                        ? card_fill_writer_set_[frameIdx] : card_fill_set_[frameIdx] };
                    cmd->BindDescriptorSets(PipelineBindPoint::Compute,
                        source_writer_audit_enabled_ ? card_fill_writer_layout_ : card_fill_layout_,
                        0, 1, fillSets, 0, nullptr);

                    // One-shot bracket around the ONLY intervening writer.
                    // No shader tags, radiance changes, or extra bindings.
                    bool auditFill = false;
                    const char* auditFrameEnv = std::getenv("SC_FILL_AUDIT_FRAME");
                    const u32 auditFrame = auditFrameEnv ? u32(std::strtoul(auditFrameEnv, nullptr, 10)) : 0u;
                    if (device_->GetPlatform() == RHIPlatform::Vulkan &&
                        std::getenv("SC_FILL_AUDIT_PREFIX") && total_texels > 0u &&
                        current_frame_index >= auditFrame &&
                        card_fill_audit_.staging == handles::INVALID_RESOURCE) {
                        BufferDesc rb{};
                        rb.size = u64(atlas_size_) * atlas_size_ * 16u;
                        rb.memoryUsage = GPUMemoryUsage::Readback;
                        rb.bindFlags = u32(BufferUsageFlags::TransferDst);
                        rb.name = "SC_CardFillBracket";
                        card_fill_audit_.staging = device_->CreateBuffer(rb);
                        card_fill_audit_.frame = current_frame_index;
                        card_fill_audit_.atlas_size = atlas_size_;
                        const auto& firstCascade = globalSDF.GetCascade(0);
                        card_fill_audit_.offline_source = offlineFill;
                        card_fill_audit_.sdf_count = cascadeCount;
                        card_fill_audit_.sdf_origin = offlineFill ? sc_offline_sdf_origin_ : firstCascade.origin;
                        card_fill_audit_.sdf_extent = offlineFill ? sc_offline_sdf_extent_ : firstCascade.extent;
                        card_fill_audit_.sdf_resolution = offlineFill ? sc_offline_sdf_resolution_ : firstCascade.resolution;
                        card_fill_audit_cards_ = cards;
                        auditFill = card_fill_audit_.staging != handles::INVALID_RESOURCE;
                        // Only explicit opt-in: record the same resource handles
                        // installed in this CardFill descriptor set above.
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
                        const char* materialAudit=std::getenv("SC_FILL_MATERIAL_AUDIT");
                        if(auditFill && materialAudit && materialAudit[0]=='1') {
                            auto* vk=static_cast<VulkanDevice*>(device_);
                            u64 instanceBytes=0,materialBytes=0;
                            auto* texture=vk->GetTexture(albedoTexArr);
                            if(vk->GetBufferByteSize(instBuf,instanceBytes) && vk->GetBufferByteSize(matBuf,materialBytes) &&
                               instanceBytes%192==0 && materialBytes%48==0 &&
                               instanceBytes<=192u*1048576u && materialBytes<=48u*1048576u && texture) {
                                BufferDesc tables{};
                                tables.size=instanceBytes+materialBytes+160;
                                tables.memoryUsage=GPUMemoryUsage::Readback;
                                tables.bindFlags=u32(BufferUsageFlags::TransferDst);
                                tables.name="SC_CardFillResidentTables";
                                card_fill_audit_.tables_staging=device_->CreateBuffer(tables);
                                if(card_fill_audit_.tables_staging!=handles::INVALID_RESOURCE) {
                                    card_fill_audit_.instance_source=instBuf;
                                    card_fill_audit_.material_source=matBuf;
                                    card_fill_audit_.albedo_source=albedoTexArr;
                                    card_fill_audit_.instance_bytes=instanceBytes;
                                    card_fill_audit_.material_bytes=materialBytes;
                                    const auto& textureDesc=texture->GetTextureDesc();
                                    card_fill_audit_.albedo_width=textureDesc.size.x;
                                    card_fill_audit_.albedo_height=textureDesc.size.y;
                                    card_fill_audit_.albedo_layers=textureDesc.arraySize;
                                    // InsertBarrier currently skips Vulkan buffers;
                                    // these read-only inputs need explicit dependencies.
                                    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::AllCommands,PipelineStage::Transfer,
                                                       AccessFlag::HostWrite|AccessFlag::MemoryWrite,AccessFlag::TransferRead);
                                    cmd->CopyBuffer(instBuf,card_fill_audit_.tables_staging,0,0,instanceBytes);
                                    cmd->CopyBuffer(matBuf,card_fill_audit_.tables_staging,0,instanceBytes,materialBytes);
                                    cmd->CopyBuffer(card_fill_cb_[frameIdx],card_fill_audit_.tables_staging,0,instanceBytes+materialBytes,160);
                                    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,
                                                       AccessFlag::TransferWrite,AccessFlag::HostRead);
                                    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::ComputeShader,
                                                       AccessFlag::TransferRead,AccessFlag::ShaderRead);
                                }
                            }
                        }
#endif
                        // Optional actual source0 snapshot, not a CPU surrogate
                        // or Map of GPU-only storage. Preserve ShaderResource
                        // state around the transfer; same retirement as bracket.
                        if (auditFill && std::getenv("SC_FILL_SDF_AUDIT")) {
                            const auto format = device_->GetTextureFormat(sdfTex[0]);
                            const u32 stride = format == DataFormat::R32_Float ? 4u :
                                (format == DataFormat::R16_Float ? 2u : 0u);
                            if (stride) {
                                const u32 res = card_fill_audit_.sdf_resolution;
                                BufferDesc sb{};
                                sb.size = u64(res) * res * res * stride;
                                sb.memoryUsage = GPUMemoryUsage::Readback;
                                sb.bindFlags = u32(BufferUsageFlags::TransferDst);
                                sb.name = "SC_CardFillSource0";
                                card_fill_audit_.sdf_staging = device_->CreateBuffer(sb);
                                card_fill_audit_.sdf_stride = stride;
                                if (card_fill_audit_.sdf_staging != handles::INVALID_RESOURCE && card_fill_audit_.sdf_stride) {
                                    ResourceBarrier toCopy{};
                                    toCopy.resource = sdfTex[0];
                                    toCopy.beforeState = ResourceState::ShaderResource;
                                    toCopy.afterState = ResourceState::CopySource;
                                    toCopy.subresource = 0xFFFFFFFF;
                                    cmd->InsertBarrier(&toCopy, 1);
                                    BufferTextureCopyRegion region{};
                                    region.bufferRowLength = res;
                                    region.imageSubresource = {0, 0, 1};
                                    region.imageExtent = {res, res, res};
                                    cmd->CopyTextureToBuffer(sdfTex[0], card_fill_audit_.sdf_staging, &region, 1);
                                    ResourceBarrier back = toCopy;
                                    back.beforeState = ResourceState::CopySource;
                                    back.afterState = ResourceState::ShaderResource;
                                    cmd->InsertBarrier(&back, 1);
                                }
                            }
                        }
                    }
                    auto copyFillAudit = [&](u32 phase) {
                        const ResourceHandle sources[] = {depth_atlas_, albedo_atlas_};
                        for (u32 i = 0; i < 2u; ++i) {
                            ResourceBarrier toCopy{};
                            toCopy.resource = sources[i];
                            toCopy.beforeState = ResourceState::UnorderedAccess;
                            toCopy.afterState = ResourceState::CopySource;
                            toCopy.subresource = 0xFFFFFFFF;
                            cmd->InsertBarrier(&toCopy, 1);
                            BufferTextureCopyRegion region{};
                            region.bufferOffset = u64(phase * 2u + i) * atlas_size_ * atlas_size_ * 4u;
                            region.bufferRowLength = atlas_size_;
                            region.imageSubresource = {0, 0, 1};
                            region.imageExtent = {atlas_size_, atlas_size_, 1};
                            cmd->CopyTextureToBuffer(sources[i], card_fill_audit_.staging, &region, 1);
                            ResourceBarrier back = toCopy;
                            back.beforeState = ResourceState::CopySource;
                            back.afterState = ResourceState::UnorderedAccess;
                            cmd->InsertBarrier(&back, 1);
                        }
                    };
                    if (auditFill) copyFillAudit(0);

                    u32 fillGroups = (total_texels + 255) / 256;
                    cmd->Dispatch(fillGroups, 1, 1);
                    if (auditFill) {
                        copyFillAudit(1);
                        card_fill_audit_.queued = true;
                    }

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
            const bool boundaryProofNow=PrepareBoundaryLightingProof(current_frame_index,frameIdx);
            {
                auto& globalSDF = nanite::GlobalSDF::Get();
                if (GIDebug().sc_shadow &&   // [F3-V4] 死旋钮接门控（visibility 保持上帧）
                    shadow_pipeline_ != handles::INVALID_PIPELINE &&
                    shadow_visibility_buffer_ != handles::INVALID_RESOURCE &&
                    (globalSDF.IsInitialized() || HasOfflineSDFSource())) {
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
                    if (HasOfflineSDFSource()) {
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

                    // The shader reads only c < cascadeCount, but Vulkan
                    // still requires every declared sampled-image descriptor
                    // to contain a valid image view. Alias unused slots to
                    // cascade 0 when the offline volume has just one level.
                    if (cascadeCount > 0) {
                        for (u32 c = 1; c < 3; ++c)
                            if (sdfTex[c] == handles::INVALID_RESOURCE) sdfTex[c] = sdfTex[0];
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
                        static_assert(sizeof(ShadowParamsGPU)==432,"boundary proof shadow parameter mirror");
                        auto* sp = static_cast<ShadowParamsGPU*>(device_->MapBuffer(shadow_cb_[frameIdx]));
                        if (sp) {
                            memset(sp, 0, sizeof(ShadowParamsGPU));
                            float resArr[3] = {0.0f, 0.0f, 0.0f};
                            if (cascadeCount == 1 &&
                                HasOfflineSDFSource()) {
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
                            {shIsVk ? 6u : 1u, DescriptorType::StorageBuffer, GetCardDispatchBuffer(frameIdx)},
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
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "SurfaceCache.Shadow", "visibility", shadow_visibility_buffer_, true);

                        if(boundaryProofNow && !BoundaryProof().failed){
                            auto& p=BoundaryProof();
                            std::vector<DescriptorData> descriptors(std::begin(shadowParams),std::end(shadowParams));
                            descriptors[8].resource=p.visibility;
                            descriptors.push_back({13,DescriptorType::StorageBuffer,p.points});
                            UpdateDescriptorSet(device_,p.sets[0],descriptors.data(),u32(descriptors.size()));
                            cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::Transfer,PipelineStage::ComputeShader,
                                AccessFlag::HostWrite|AccessFlag::TransferWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
                            cmd->BindComputePipeline(p.pipelines[0]);cmd->BindDescriptorSets(PipelineBindPoint::Compute,p.layouts[0],0,1,&p.sets[0],0,nullptr);
                            BoundaryTimestamp(cmd,0);DispatchScNativeLabel(device_,cmd,"SC.boundary.shadow",(p.count+255)/256,1,1);BoundaryTimestamp(cmd,1);p.shadowed=true;
                        }
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
            // [F3-V4] sc_lighting 门控：false = 跳过 dispatch（lighting atlas
            // 保持上帧内容）。GI_ALL_OFF 的 direct-only A/B 语义由此生效。
            if (GIDebug().sc_lighting) {
                const bool isVk = device_->GetPlatform() == rhi::RHIPlatform::Vulkan;
                // feedbackIdx = 2 frames ago — the lighting atlas from 2
                // frames back is safe to read (GPU finished with it) and
                // remains the multi-bounce feedback/query source. Phase 1d:
                // (N+1)%3 与 DDGISlotMap 恒等（N-2 ≡ N+1 mod 3，+1 写法避免
                // unsigned 下溢），槽位语义以 DDGISlotMap.h 为准。
                const u32 feedbackIdx = DDGISlotsForFrame(current_frame_index + 1u).currentIndex;
                // temporalPrevIdx = the most recently completed frame (N-1).
                // This is intentionally a separate history clock for the tile
                // solver's indirect_out EMA and tile-grid EMA/carry. Reusing
                // feedbackIdx here splits partial-refresh history by parity and
                // prevents non-refreshed tiles from carrying the latest frame.
                const u32 temporalPrevIdx = DDGISlotsForFrame(current_frame_index).previousIndex;
                // [E0-B 2026-09-21 + 780 帧稳定性修正] prev_light（legacy 自反馈）
                // 必须继续读混合 atlas：自反馈回路每代重注入 fresh 直射
                //（direct 逐帧解析计算），是 SC 场的能量源；若改读 nodirect，
                // 每代递归剥一次直射 → 指数衰减 → frame 780 全图坍黑（实测
                // meanY 64.6→9.4，B14 "indirect fading" 的回路机制复现）。
                // nodirect 副本因此只做探针/查询侧的"静态直射项隔离"——
                // result 本体（含 prev 直射库）稳定，nodirect = result −
                // direct·ρ/π 同样稳定。
                const ResourceHandle prevLightingAtlas = lighting_atlas_[feedbackIdx];
                // Vulkan 17 项 / Metal 15 项（[E0-B] nodirect 输出各 +1）。
                DescriptorData evalParams[18];
                u32 evalParamCount = 0;
                // [WRC 计划 Stream D2]: 页表缓冲（页表模式用真表；关闭时绑
                // stand-in，flag=0 shader 不读）。
                const bool ptOn = card_generator_.IsPageTableEnabled();
                ResourceHandle pageTableBuf = ptOn
                    ? card_generator_.GetPageTableBuffer()
                    : ResourceHandle(GetCardDispatchBuffer(frameIdx));
                if (isVk) {
                    DescriptorData p[] = {
                        {0, DescriptorType::StorageImage,  albedo_atlas_},
                        {1, DescriptorType::StorageImage,  normal_atlas_},
                        {2, DescriptorType::StorageImage,  emissive_atlas_},
                        {3, DescriptorType::StorageImage,  lighting_atlas_[outIdx]},
                        {4, DescriptorType::UniformBuffer, params_cb_[frameIdx]},
                        {5, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()},
                        {6, DescriptorType::StorageBuffer, GetLightInfoBuffer(frameIdx)},
                        {7, DescriptorType::StorageBuffer, GetCardDispatchBuffer(frameIdx)},
                        {8, DescriptorType::StorageImage,  prevLightingAtlas},  // prev for multi-bounce [E0-B: nodirect]
                        {9, DescriptorType::StorageBuffer, shadow_visibility_buffer_},  // T3.3
                        // Probe-fed indirect: bind the previous-frame slot
                        // even when invalid (CB flag gates the shader term).
                        {10, DescriptorType::StorageBuffer,
                         frame_data.ddgi_irradiance != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_irradiance
                             : GetCardDispatchBuffer(frameIdx)},
                        // Surface-point reconstruction for the probe gather
                        // (same depth source the shadow pass uses).
                        {11, DescriptorType::StorageImage,  depth_atlas_},
                        {12, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]},   // T3.3 tile radiosity
                        // C 系列：探针深度（Chebyshev gather 可见性）；无效时绑
                        // stand-in（CB flag 关 → shader 不读）。
                        {13, DescriptorType::StorageBuffer,
                         frame_data.ddgi_depth != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_depth
                             : GetCardDispatchBuffer(frameIdx)},
                        // GI 根治 Phase 3.2: openSky 统计（无效时绑 stand-in，
                        // radiosity_gate.z 关 → shader 不读）。
                        {14, DescriptorType::StorageBuffer,
                         frame_data.ddgi_opensky != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_opensky
                             : GetCardDispatchBuffer(frameIdx)},
                        // [WRC 计划 Stream D2]: 卡片页表（过表寻址）。
                        {15, DescriptorType::StorageBuffer, pageTableBuf},
                        // [E0-B 2026-09-21] nodirect 副本输出（result − direct·ρ/π）。
                        {16, DescriptorType::StorageImage,  lighting_nodirect_atlas_[outIdx]},
                    };
                    memcpy(evalParams, p, sizeof(p));
                    evalParamCount = (u32)std::size(p);
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
                        {3, DescriptorType::StorageBuffer, GetLightInfoBuffer(frameIdx)},
                        {4, DescriptorType::StorageBuffer, GetCardDispatchBuffer(frameIdx)},
                        {5, DescriptorType::StorageBuffer, shadow_visibility_buffer_},  // T3.3
                        {4, DescriptorType::StorageImage,  prevLightingAtlas},   // prev for multi-bounce [E0-B: nodirect]
                        {5, DescriptorType::StorageImage,  depth_atlas_},               // surface-point probe gather
                        // Probe-fed indirect: bind the previous-frame slot even
                        // when invalid (CB flag gates the shader term).
                        {6, DescriptorType::StorageBuffer,
                         frame_data.ddgi_irradiance != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_irradiance
                             : GetCardDispatchBuffer(frameIdx)},
                        // T3.3 tile radiosity (Metal texture namespace 6)
                        {6, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]},
                        // C 系列：探针深度（Chebyshev gather 可见性）
                        {7, DescriptorType::StorageBuffer,
                         frame_data.ddgi_depth != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_depth
                             : GetCardDispatchBuffer(frameIdx)},
                        // GI 根治 Phase 3.2: openSky 统计（Metal buffer(8)）
                        {8, DescriptorType::StorageBuffer,
                         frame_data.ddgi_opensky != handles::INVALID_RESOURCE
                             ? frame_data.ddgi_opensky
                             : GetCardDispatchBuffer(frameIdx)},
                        // [E0-B 2026-09-21] nodirect 副本输出 [[texture(7)]]。
                        {7, DescriptorType::StorageImage,  lighting_nodirect_atlas_[outIdx]},
                    };
                    memcpy(evalParams, p, sizeof(p));
                    evalParamCount = (u32)std::size(p);
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

                UpdateDescriptorSet(device_, light_eval_set_[frameIdx], evalParams, evalParamCount);

                // Explicit GENERAL transition for the lighting atlases — do not
                // rely on the render graph's Write declaration having put them
                // in a storage-compatible layout before this dispatch. Both the
                // written slot (outIdx) and the multi-bounce feedback slot
                // (feedbackIdx) are bound as storage images; feedbackIdx is not RG-
                // declared, and on its first frames it is still UNDEFINED.
                // The Vulkan backend resolves oldLayout from the tracked layout,
                // so the first such barrier correctly performs UNDEFINED→UAV.
                {
                    // P0-B（01197 定位）: indirect_out[outIdx] 也在 LightEval
                    // 的 storage 写列表（binding 12）——上一帧它作为 resolve 的
                    // EMA history 被 sampled（tracked=SRO），写前必须转回
                    // GENERAL；否则 storage 写在 SRO 上发生（UB，validation 钉
                    // real=GENERAL），后续 resolve 前置 UA barrier 触发 01197。
                    // [E0-B] nodirect 两个槽（outIdx 写 + feedbackIdx 作为
                    // prev_lighting 的 imageLoad/resolve 采样源）与混合 atlas
                    // 完全同构地显式转 GENERAL——barrier 覆盖必须对称。
                    ResourceBarrier lb[5]{};
                    lb[0].resource = lighting_atlas_[outIdx];
                    lb[1].resource = lighting_atlas_[feedbackIdx];
                    lb[2].resource = indirect_out_tex_[outIdx];
                    lb[3].resource = lighting_nodirect_atlas_[outIdx];
                    lb[4].resource = lighting_nodirect_atlas_[feedbackIdx];
                    for (auto& b : lb) {
                        b.beforeState = ResourceState::UnorderedAccess;
                        b.afterState  = ResourceState::UnorderedAccess;
                        b.subresource = 0xFFFFFFFF;
                    }
                    cmd->InsertBarrier(lb, 5);
                }

                // Exact Vulkan/DUMP2 opt-in: copy actual inputs immediately
                // before this writer and outputs immediately after it. No replay
                // shader, production binding, or production-value modification.
                const char* snapshotEnv = std::getenv("DDGI_SC_WRITER_SNAPSHOT");
                const char* snapshotDump = std::getenv("DDGI_DUMP");
                const char* snapshotFrame = std::getenv("DDGI_DUMP_FRAME");
                const bool snapshotNow = isVk && snapshotEnv && strcmp(snapshotEnv, "1") == 0 &&
                    snapshotDump && strcmp(snapshotDump, "2") == 0 && snapshotFrame &&
                    current_frame_index == u32(std::strtoul(snapshotFrame, nullptr, 10)) &&
                    !light_writer_snapshot_.recorded && !light_writer_snapshot_.failed;
                auto snapshotTexture = [&](u32 index) {
                    ResourceBarrier toCopy{};
                    toCopy.resource = light_writer_snapshot_.sources[index];
                    toCopy.beforeState = ResourceState::Unknown;
                    toCopy.afterState = ResourceState::CopySource;
                    toCopy.subresource = 0xFFFFFFFF;
                    cmd->InsertBarrier(&toCopy, 1);
                    BufferTextureCopyRegion region{};
                    region.bufferOffset = light_writer_snapshot_.offsets[index];
                    region.bufferRowLength = atlas_size_;
                    region.imageSubresource = {0, 0, 1};
                    region.imageExtent = {atlas_size_, atlas_size_, 1};
                    cmd->CopyTextureToBuffer(toCopy.resource, light_writer_snapshot_.staging, &region, 1);
                    toCopy.beforeState = ResourceState::CopySource;
                    toCopy.afterState = ResourceState::UnorderedAccess;
                    cmd->InsertBarrier(&toCopy, 1);
                };
                if (snapshotNow) {
                    auto& witness = light_writer_snapshot_;
                    witness.frame = current_frame_index;
                    witness.render_slot = frameIdx;
                    witness.output_slot = outIdx;
                    witness.dispatch_cards = card_count;
                    const ResourceHandle sources[] = {indirect_out_tex_[outIdx], albedo_atlas_, normal_atlas_,
                        depth_atlas_, emissive_atlas_, shadow_visibility_buffer_, params_cb_[frameIdx],
                        GetLightInfoBuffer(frameIdx), card_generator_.GetCardDataBuffer(), GetCardDispatchBuffer(frameIdx),
                        lighting_atlas_[outIdx], lighting_nodirect_atlas_[outIdx]};
                    const u64 texels = u64(atlas_size_) * atlas_size_;
                    const u64 sizes[] = {texels*8,texels*4,texels*4,texels*4,texels*8,texels*4,
                        sizeof(FlattenedLightingParams),4096,
                        u64(card_generator_.GetCardCount())*sizeof(SurfaceCacheCard),
                        u64(card_count)*sizeof(CardDispatchInfo),texels*8,texels*8};
                    for (u32 i = 0; i < 12; ++i) {
                        witness.sources[i] = sources[i];
                        witness.offsets[i] = witness.bytes;
                        witness.sizes[i] = sizes[i];
                        witness.bytes += (sizes[i] + 15u) & ~u64(15u);
                        if (sources[i] == handles::INVALID_RESOURCE || !sizes[i]) witness.failed = true;
                    }
                    if (witness.bytes > 256ull*1024ull*1024ull) witness.failed = true;
                    if (!witness.failed) {
                        BufferDesc desc{};
                        desc.size = witness.bytes;
                        desc.memoryUsage = GPUMemoryUsage::Readback;
                        desc.bindFlags = u32(BufferUsageFlags::TransferDst);
                        desc.name = "SC_LightWriterInputsOutputs";
                        witness.staging = device_->CreateBuffer(desc);
                        witness.failed = witness.staging == handles::INVALID_RESOURCE;
                    }
                    if (!witness.failed) {
                        // Vulkan InsertBarrier skips buffers. Authenticate the
                        // Host/compute producer and transfer copies explicitly.
                        cmd->MemoryBarrier(PipelineStage::Host | PipelineStage::AllCommands, PipelineStage::Transfer,
                            AccessFlag::HostWrite | AccessFlag::MemoryWrite, AccessFlag::TransferRead);
                        for (u32 i = 0; i < 5; ++i) snapshotTexture(i);
                        for (u32 i = 5; i < 10; ++i)
                            cmd->CopyBuffer(witness.sources[i], witness.staging, 0, witness.offsets[i], witness.sizes[i]);
                        cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::ComputeShader,
                            AccessFlag::TransferRead, AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
                    } else {
                        PRIMAL_LOG_STREAM(Error, "Render") << "[SC_LIGHT_WRITER_SNAPSHOT] allocation/source failed" << std::endl;
                    }
                }
                cmd->BindComputePipeline(light_eval_pipeline_);
                const DescriptorSetHandle sets[] = { light_eval_set_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, light_eval_layout_, 0, 1, sets, 0, nullptr);

                u32 groups = (total_texels + 255) / 256;
                cmd->Dispatch(groups, 1, 1);
                if (lumencompare::enabled) {
                    lumencompare::Output(device_, cmd, "SurfaceCache.Policy", "params", params_cb_[frameIdx], true);
                    lumencompare::Output(device_, cmd, "SurfaceCache.Policy", "dispatch", GetCardDispatchBuffer(frameIdx), true);
                    lumencompare::Output(device_, cmd, "SurfaceCache.Policy", "cards", card_generator_.GetCardDataBuffer(), true);
                    const auto directory = std::filesystem::path(std::getenv("LUMEN_COMPARE_DIR")) /
                        ("frame-" + std::to_string(lumencompare::ordinal));
                    std::filesystem::create_directories(directory);
                    std::ofstream policy(directory / "surface-policy.json");
                    const bool legacy = device_->GetPlatform() == RHIPlatform::Vulkan && LegacySurfacePolicy();
                    const bool fillOffline = HasOfflineSDFSource() && (!legacy ||
                        (std::getenv("SC_FILL_OFFLINE_SDF") && std::getenv("SC_FILL_OFFLINE_SDF")[0] == '1'));
                    policy << "{\"frame\":" << current_frame_index << ",\"legacy_surface\":" << (legacy ? "true" : "false")
                        << ",\"allocation_epoch\":" << card_generator_.GetAllocationEpoch()
                        << ",\"card_count\":" << card_generator_.GetCardCount()
                        << ",\"qualified_writers\":\"owner_raster_or_offline_page\""
                        << ",\"unqualified_SDF_surface_valid\":" << (legacy ? "true" : "false")
                        << ",\"fill_offline\":" << (fillOffline ? "true" : "false")
                        << ",\"offline_resolution\":" << sc_offline_sdf_resolution_
                        << ",\"offline_signed\":" << (sc_offline_sdf_signed_ ? "true" : "false") << "}\n";
                }
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "SurfaceCache.LightEval", "lighting", lighting_atlas_[outIdx], false);
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "SurfaceCache.LightEval", "albedo", albedo_atlas_, false);
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "SurfaceCache.LightEval", "normal", normal_atlas_, false);
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "SurfaceCache.LightEval", "depth", depth_atlas_, false);
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "SurfaceCache.LightEval", "emissive", emissive_atlas_, false);
                if (snapshotNow && !light_writer_snapshot_.failed) {
                    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::Transfer,
                        AccessFlag::ShaderWrite, AccessFlag::TransferRead);
                    snapshotTexture(10);
                    snapshotTexture(11);
                    cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                        AccessFlag::TransferWrite, AccessFlag::HostRead);
                    cmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::ComputeShader,
                        AccessFlag::TransferRead, AccessFlag::ShaderRead | AccessFlag::ShaderWrite);
                    light_writer_snapshot_.recorded = true;
                }

                if(boundaryProofNow && BoundaryProof().shadowed && !BoundaryProof().failed){
                    auto& p=BoundaryProof();
                    std::vector<DescriptorData> descriptors(evalParams,evalParams+evalParamCount);
                    for(auto& d:descriptors)if(d.binding==9)d.resource=p.visibility;
                    descriptors.push_back({17,DescriptorType::StorageBuffer,p.points});
                    descriptors.push_back({18,DescriptorType::StorageBuffer,p.results});
                    UpdateDescriptorSet(device_,p.sets[1],descriptors.data(),u32(descriptors.size()));
                    cmd->MemoryBarrier(PipelineStage::ComputeShader|PipelineStage::Host,PipelineStage::ComputeShader,
                        AccessFlag::ShaderWrite|AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
                    cmd->BindComputePipeline(p.pipelines[1]);cmd->BindDescriptorSets(PipelineBindPoint::Compute,p.layouts[1],0,1,&p.sets[1],0,nullptr);
                    BoundaryTimestamp(cmd,2);DispatchScNativeLabel(device_,cmd,"SC.boundary.light",(p.count+255)/256,1,1);BoundaryTimestamp(cmd,3);
                    if(p.readback_requested){
                    const u64 probes=u64(frame_data.ddgi_counts[0])*frame_data.ddgi_counts[1]*frame_data.ddgi_counts[2];
                    ResourceHandle sources[]={p.points,p.results,p.visibility,params_cb_[frameIdx],shadow_cb_[frameIdx],GetLightInfoBuffer(frameIdx),
                        frame_data.ddgi_irradiance,frame_data.ddgi_depth,frame_data.ddgi_opensky,card_generator_.GetCardDataBuffer()};
                    const u64 sizes[]={u64(p.count)*64,u64(p.count)*64,u64(p.count)*4,sizeof(FlattenedLightingParams),432,4096,
                        sources[6]!=handles::INVALID_RESOURCE?probes*9*16:0,
                        sources[7]!=handles::INVALID_RESOURCE?probes*128*4:0,
                        sources[8]!=handles::INVALID_RESOURCE?probes*2*4:0,u64(card_generator_.GetCardCount())*sizeof(SurfaceCacheCard)};
                    if(probes>131072)p.failed=true;
                    for(u32 i=0;i<10;++i){p.offsets[i]=p.bytes;p.sizes[i]=sizes[i];p.bytes+=(sizes[i]+15)&~u64(15);}
                    if(p.bytes>128ull*1024*1024)p.failed=true;
                    if(!p.failed){BufferDesc d{};d.size=p.bytes;d.memoryUsage=d.usage=GPUMemoryUsage::Readback;
                        if(p.staging!=handles::INVALID_RESOURCE && p.staging_capacity!=p.bytes){device_->DestroyBuffer(p.staging);p.staging=handles::INVALID_RESOURCE;}
                        p.staging_capacity=p.bytes;
                        if(p.staging==handles::INVALID_RESOURCE)p.staging=device_->CreateBuffer(d);p.failed=p.staging==handles::INVALID_RESOURCE;}
                    if(!p.failed){
                        cmd->MemoryBarrier(PipelineStage::ComputeShader|PipelineStage::Host,PipelineStage::Transfer,
                            AccessFlag::ShaderWrite|AccessFlag::HostWrite,AccessFlag::TransferRead);
                        for(u32 i=0;i<10;++i)if(sizes[i])cmd->CopyBuffer(sources[i],p.staging,0,p.offsets[i],sizes[i]);
                        cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
                        cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::ComputeShader,
                            AccessFlag::TransferRead,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
                        p.recorded=true;
                        PRIMAL_LOG_STREAM(Info,"Render")<<"[SC_BOUNDARY_LIGHT_PROOF] recorded points="<<p.count<<" frame="<<p.frame<<std::endl;
                    }
                    }else p.recorded=true;
                }
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
                { static bool sReach2 = false;
                  if (!sReach2) { sReach2 = true;
                    PRIMAL_LOG_STREAM(Error, "Render")
                      << "[SC_P4] indirect block: on=" << scIndirectOn
                      << " gi_frame=" << frame_data.gi_frame
                      << " warmup=" << scWarmupFrames() << std::endl; } }
                if (scIndirectOn && frame_data.gi_frame >= scWarmupFrames())
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

                    if(geometry_first_requested_ && (ray_triangle_buffer_==handles::INVALID_RESOURCE || ray_triangle_capacity_!=kTotalHits)) {
                        if(ray_triangle_buffer_!=handles::INVALID_RESOURCE) device_->DestroyBuffer(ray_triangle_buffer_);
                        BufferDesc desc{}; desc.size=kTotalHits*sizeof(u32); desc.type=BufferType::Structured;
                        desc.memoryUsage=desc.usage=GPUMemoryUsage::Dynamic;
                        desc.structured.elementCount=u32(kTotalHits); desc.structured.elementStride=sizeof(u32);
                        ray_triangle_buffer_=device_->CreateBuffer(desc); ray_triangle_capacity_=kTotalHits;
                        if(ray_triangle_buffer_!=handles::INVALID_RESOURCE) {
                            if(void* dst=device_->MapBuffer(ray_triangle_buffer_)) {
                                std::memset(dst,255,desc.size); device_->UnmapBuffer(ray_triangle_buffer_);
                            } else kTilesEff=0;
                        } else kTilesEff=0;
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

                    // Triple-buffered exact owner lookup. Rebuild on atlas allocation
                    // epoch, not merely card count: same-count relocation changes ownership.
                    const u64 ownerBytes = u64(kTiles)*kTiles*sizeof(u32);
                    const u64 ownerSig = (u64(card_generator_.GetAllocationEpoch())<<32) ^ kTile;
                    auto& ownerBuffer = indirect_tile_owner_buffers_[frameIdx];
                    if (ownerBuffer == handles::INVALID_RESOURCE || indirect_tile_owner_bytes_[frameIdx] != ownerBytes) {
                        if (ownerBuffer != handles::INVALID_RESOURCE) device_->DestroyBuffer(ownerBuffer);
                        BufferDesc desc{}; desc.size=ownerBytes; desc.type=BufferType::Structured;
                        desc.memoryUsage=desc.usage=GPUMemoryUsage::Dynamic;
                        desc.structured.elementCount=kTiles*kTiles; desc.structured.elementStride=sizeof(u32);
                        ownerBuffer=device_->CreateBuffer(desc);
                        indirect_tile_owner_bytes_[frameIdx]=ownerBytes;
                        indirect_tile_owner_signatures_[frameIdx]=~u64(0);
                    }
                    if (indirect_tile_owner_signatures_[frameIdx] != ownerSig) {
                        std::vector<u32> owners;
                        const auto& liveCards=card_generator_.GetCards();
                        const bool valid=BuildCardTileOwners(std::span<const SurfaceCacheCard>(liveCards),atlas_size_,kTile,owners);
                        if (!valid) {
                            owners.assign(u64(kTiles)*kTiles,0xFFFFFFFFu);
                            PRIMAL_LOG_STREAM(Error,"Render") << "[SC_TILE_OWNER] invalid/overlapping atlas placement";
                        }
                        auto* dst=ownerBuffer!=handles::INVALID_RESOURCE ? device_->MapBuffer(ownerBuffer) : nullptr;
                        if (dst) {
                            std::memcpy(dst,owners.data(),ownerBytes); device_->UnmapBuffer(ownerBuffer);
                            indirect_tile_owner_signatures_[frameIdx]=ownerSig;
                        } else kTilesEff=0;
                    }

                    // C3.5 slice 4: offline propagated SDF for the tile march
                    // (thin-arch sky leaks in the coarse runtime GlobalSDF).
                    const bool useOfflineSdf = HasOfflineSDFSource();

                    auto& gsdf = nanite::GlobalSDF::Get();

                    const bool geometryReady=geometry_first_requested_ && UploadGeometryScene(frameIdx);

                    // ---- trace params upload ----
                    if (auto* tp = static_cast<IndirectTraceParamsGPU*>(
                            device_->MapBuffer(indirect_trace_params_cb_[frameIdx]))) {
                        *tp = IndirectTraceParamsGPU{};
                        tp->sc_params.atlas_size = atlas_size_;
                        tp->sc_params.page_size = page_size_;
                        tp->sc_params.max_cards = u32(card_generator_.GetCards().size());
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
                        const char* nonzero=std::getenv("PRIMAL_SC_BVH_NONZERO_AXIS");
                        tp->sdf_cascade_count = math::u32v4{cc, geometryReady?1u:0u, 0,
                            isVk && geometryReady && nonzero && std::strcmp(nonzero,"1")==0?1u:0u};
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
                        DescriptorData ip[18];
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
                        ip[12] = {isVk ? 12u : 4u, DescriptorType::StorageBuffer, ownerBuffer};
                        if(geometry_first_requested_) for(u32 g=0;g<4;++g)
                            ip[13+g]={13+g,DescriptorType::StorageBuffer,geometry_buffers_[frameIdx].buffers[g]!=handles::INVALID_RESOURCE?geometry_buffers_[frameIdx].buffers[g]:ownerBuffer};
                        if(geometry_first_requested_) ip[17]={17,DescriptorType::StorageBuffer,ray_triangle_buffer_!=handles::INVALID_RESOURCE?ray_triangle_buffer_:ownerBuffer};
                        UpdateDescriptorSet(device_, indirect_trace_set_[frameIdx], ip, geometry_first_requested_?18u:13u);
                        cmd->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead);
                        cmd->BindComputePipeline(indirect_trace_pipeline_);
                        const DescriptorSetHandle isets[] = { indirect_trace_set_[frameIdx] };
                        cmd->BindDescriptorSets(PipelineBindPoint::Compute, indirect_trace_layout_, 0, 1, isets, 0, nullptr);
                        DispatchScNativeLabel(device_,cmd,"SC.main.trace",kTilesEff,kTilesEff,1);
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "SurfaceCache.IndirectTrace", "ray-hits", ray_hits_buffer_, true);
                        if(boundaryProofNow && isVk && geometry_first_requested_){
                            auto& p=BoundaryProof();
                            std::vector<DescriptorData> descriptors(ip,ip+18);
                            descriptors[8].resource=p.radio_buffers[0];descriptors[17].resource=p.radio_buffers[1];
                            descriptors.push_back({18,DescriptorType::StorageBuffer,p.points});
                            descriptors.push_back({19,DescriptorType::StorageBuffer,p.radio_buffers[2]});
                            std::vector<DescriptorSetLayoutBinding> bindings;
                            for(const auto& d:descriptors){DescriptorSetLayoutBinding b{d.binding,d.type,1,ShaderStage::Compute,nullptr};
                                b.is3D=(d.binding>=2 && d.binding<=4)||(d.binding>=9 && d.binding<=11);bindings.push_back(b);}
                            if(PrepareBoundaryRadiosityProof(2,bindings)){
                                descriptors[8].resource=p.radio_buffers[0];descriptors[17].resource=p.radio_buffers[1];descriptors[19].resource=p.radio_buffers[2];
                                UpdateDescriptorSet(device_,p.sets[2],descriptors.data(),u32(descriptors.size()));
                                cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::Transfer,PipelineStage::ComputeShader,AccessFlag::HostWrite|AccessFlag::TransferWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
                                cmd->BindComputePipeline(p.pipelines[2]);cmd->BindDescriptorSets(PipelineBindPoint::Compute,p.layouts[2],0,1,&p.sets[2],0,nullptr);
                                BoundaryTimestamp(cmd,4);DispatchScNativeLabel(device_,cmd,"SC.boundary.trace",(p.count+63)/64,1,1);BoundaryTimestamp(cmd,5);p.radio_traced=true;
                            }
                        }
                        // Vulkan InsertBarrier skips buffers. Publish actual first-hit
                        // writes before resolve reads them in the next compute dispatch.
                        cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,
                            AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
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
                        // [WRC 计划 Stream D2]: resolve 查询契约过表寻址开关。
                        rp->page_table_enabled =
                            card_generator_.IsPageTableEnabled() ? 1u : 0u;
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
                        // [F0 2026-09-20 能量不对称修复 A1] tile 天空档接线：
                        // 与探针路径（DDGI/SPGI/WRC）同一 GIDebug() 旋钮驱动。
                        // [K-1 2026-09-21 E0-K] 基色从旧梯度 (2.75,2.88,3.00)
                        // 收口到探针天空单一事实源 EvaluateSkyBase：shader 侧
                        // 逐射线方向求值（tileRayDir 上仰更亮，与 SPGI/DDGI/
                        // WRC miss 同源同色），cpu 只传档位 + 方向化使能位
                        // (w=1)。残差 ~2×（[能量-排查] §3.3，A1 半程）由此消除。
                        // env PRIMAL_SC_TILE_SKY_OVERRIDE="r,g,b" 回退常量基色
                        // 路径（w=0，旧语义；旧观感传 "1.1,1.152,1.2"）。
                        {
                            const float tier = GIDebug().sky_color_intensity;
                            math::v4 sky{tier, tier, tier, 1.0f};
                            if (const char* e = std::getenv("PRIMAL_SC_TILE_SKY_OVERRIDE")) {
                                float r, g, b;
                                if (std::sscanf(e, "%f , %f , %f", &r, &g, &b) == 3) {
                                    sky = math::v4{r, g, b, 0.0f};
                                }
                            }
                            rp->tile_sky_color = sky;
                        }
                        // [稳定性 2026-09-21] tile 增长率钳制
                        //（PRIMAL_SC_TILE_GROWTH_CAP，默认 0.5；0 = 关）。
                        rp->tile_growth_cap = [] {
                            float cap = 0.5f;
                            if (const char* e = std::getenv("PRIMAL_SC_TILE_GROWTH_CAP")) {
                                const float v = static_cast<float>(std::atof(e));
                                if (v >= 0.0f && v <= 16.0f) cap = v;
                            }
                            return cap;
                        }();
                        // [SC per-mesh 计划 P4]（PRIMAL_SC_RAY_LOOKUP=1，默认关；
                        // P6 完整门禁通过才考虑翻默认）。
                        rp->ray_max_distance = 50.0f; // same segment as IndirectTrace
                        rp->ray_lookup = [] {
                            static const int k = [] {
                                const char* e = std::getenv("PRIMAL_SC_RAY_LOOKUP");
                                return e && e[0] == '1' ? 1 : 0;
                            }();
                            return (uint32_t)k;
                        }();
                        if (const char* audit = std::getenv("SC_RAY_ORACLE_PREFIX");
                            audit && audit[0] && device_->GetPlatform() == rhi::RHIPlatform::Vulkan)
                            rp->ray_lookup |= 2u;
                        if (const char* dda = std::getenv("PRIMAL_SC_RAY_DDA");
                            dda && std::strcmp(dda,"1") == 0 && device_->GetPlatform() == rhi::RHIPlatform::Vulkan)
                            rp->ray_lookup |= 4u; // experimental; first-hit/image gates currently fail
                        if(geometry_first_requested_) rp->ray_lookup=(rp->ray_lookup & ~5u)|8u;
                        if(geometry_first_requested_ && !card_generator_.IsPageTableEnabled() && triangle_support_buffer_!=handles::INVALID_RESOURCE && triangle_support_epoch_==static_composite_epoch_ && triangle_support_epoch_==card_generator_.GetAllocationEpoch()) {
                            const char* filter=std::getenv("PRIMAL_SC_CARD_TRIANGLE_FILTER");if(filter && std::strcmp(filter,"1")==0)rp->ray_lookup|=8192u;
                            const char* component=std::getenv("PRIMAL_SC_POINT_COMPONENT_REJECT");if(component && std::strcmp(component,"1")==0)rp->ray_lookup|=262144u;
                            const char* sourceBinary=std::getenv("PRIMAL_SC_SOURCE_BINARY_INSERT");if(sourceBinary && std::strcmp(sourceBinary,"1")==0)rp->ray_lookup|=131072u;
                            const char* binary=std::getenv("PRIMAL_SC_BOUNDARY_BINARY_INSERT");if(binary && std::strcmp(binary,"1")==0)rp->ray_lookup|=65536u;
                            const char* mask=std::getenv("PRIMAL_SC_OWNER_TRIANGLE_MASK");if((rp->ray_lookup&8192u)!=0u && mask && std::strcmp(mask,"1")==0)rp->ray_lookup|=32768u;
                        }
                        if(geometry_first_requested_) {
                            const char* interpolate=std::getenv("PRIMAL_SC_TRIANGLE_INTERPOLATION");
                            const bool requested=interpolate ? std::strcmp(interpolate,"1")==0
                                : StaticPagesEnabled() && card_generator_.GetStaticCardCount()!=0u;
                            if(requested) rp->ray_lookup|=16u;
                            const char* simplex=std::getenv("PRIMAL_SC_TRIANGLE_SIMPLEX");
                            if(requested && (!simplex || std::strcmp(simplex,"1")==0)) rp->ray_lookup|=32u;
                            const char* segment=std::getenv("PRIMAL_SC_TRIANGLE_SEGMENT");
                            if(requested && (!segment || std::strcmp(segment,"1")==0)) rp->ray_lookup|=64u;
                            const char* wide=std::getenv("PRIMAL_SC_TRIANGLE_WIDE_SEGMENT");
                            if(requested && (!wide || std::strcmp(wide,"1")==0)) rp->ray_lookup|=128u;
                            const char* hull=std::getenv("PRIMAL_SC_TRIANGLE_HULL");
                            if(requested && (!hull || std::strcmp(hull,"1")==0)) rp->ray_lookup|=256u;
                            const char* indexed=std::getenv("PRIMAL_SC_TRIANGLE_SUPPORT");
                            if(requested && !card_generator_.IsPageTableEnabled() && (!indexed || std::strcmp(indexed,"1")==0) && triangle_support_buffer_!=handles::INVALID_RESOURCE && triangle_support_epoch_==card_generator_.GetAllocationEpoch() && triangle_support_epoch_==static_composite_epoch_) rp->ray_lookup|=512u;
                        }
                        if(const char* ranges=std::getenv("PRIMAL_SC_OWNER_CARD_RANGES");isVk && geometry_first_requested_ && ranges && std::strcmp(ranges,"1")==0 &&
                            ValidateOwnerCardRanges(card_generator_.GetCards(),card_generator_.GetLookups())){
                            rp->ray_lookup|=2048u;
                            if(card_generator_.HasOwnerCardDirectory())rp->ray_lookup|=4096u;
                        }
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
                            // feedbackIdx remains the N-2 lighting source;
                            // temporalPrevIdx is the N-1 tile EMA/carry source.
                            resolveBarriers[0].resource = lighting_atlas_[feedbackIdx];
                            resolveBarriers[1].resource = depth_atlas_;
                            resolveBarriers[2].resource = normal_atlas_;
                            resolveBarriers[3].resource = indirect_out_tex_[temporalPrevIdx];
                            resolveBarriers[4].resource = indirect_tile_tex_[temporalPrevIdx];
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
                            // Eight resources are initialized above: five SR
                            // inputs, two per-frame UAV outputs, and the
                            // single-buffered tile_aux UAV. Keep the count in
                            // sync so tile_aux is not silently omitted.
                            cmd->InsertBarrier(resolveBarriers, 8);
                        }
                        DescriptorData rp2[21]{};
                        const bool ptOn = card_generator_.IsPageTableEnabled();
                        const ResourceHandle pageTableBuf = ptOn
                            ? card_generator_.GetPageTableBuffer()
                            : ResourceHandle(GetCardDispatchBuffer(frameIdx));
                        // [E0-B 2026-09-21 + 780 帧稳定性修正] tile 反弹的 SC
                        // 查询源保持混合 atlas：tile 环路（radiosity）的能量源
                        // 是每帧 fresh 解析直射烘进 atlas 的 L_o（受光面反弹 =
                        // 合法一跳）；若读 nodirect 则 tile 环同样指数衰减
                        //（与 prev_light 同机制，frame 780 坍黑实测复现）。
                        // nodirect 的消费端收敛为 SPGI finalize 查询（静态直射
                        // 项隔离）。PRIMAL_SC_NODIRECT=0 现只回滚 SPGI 侧。
                        static const bool kScTileNoDirect = [] {
                            const char* e = std::getenv("PRIMAL_SC_NODIRECT");
                            return !(e && e[0] == '0');
                        }();
                        (void)kScTileNoDirect;
                        const ResourceHandle tileQuerySource = lighting_atlas_[feedbackIdx];
                        if (device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
                            rp2[0] = {0, DescriptorType::SampledImage,  tileQuerySource};
                            rp2[1] = {1, DescriptorType::StorageImage,  albedo_atlas_};
                            rp2[2] = {2, DescriptorType::SampledImage,  indirect_dummy_tex_};   // dummy (unused)
                            rp2[3] = {3, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]};
                            rp2[4] = {4, DescriptorType::UniformBuffer, indirect_dummy_cb_};
                            rp2[5] = {5, DescriptorType::UniformBuffer, indirect_resolve_params_cb_[frameIdx]};
                            rp2[6] = {6, DescriptorType::StorageBuffer, card_generator_.GetCardLookupBuffer()};
                            rp2[7] = {7, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()};
                            rp2[8] = {8, DescriptorType::StorageBuffer, ray_hits_buffer_};
                            rp2[9] = {9, DescriptorType::SampledImage,  indirect_out_tex_[temporalPrevIdx]};   // T3.3 EMA history (N-1)
                            // B: per-tile validity bitmask (resolve writes the
                            // bit for tiles it consumed current hits for; the
                            // INVALID stand-in keeps the binding valid when
                            // tile radiosity is resourced off).
                            rp2[10] = {10, DescriptorType::StorageBuffer,
                                       indirect_tile_valid_buffer_ != handles::INVALID_RESOURCE
                                           ? indirect_tile_valid_buffer_
                                           : GetCardDispatchBuffer(frameIdx)};
                            // C: capture validation + orientation atlases.
                            rp2[11] = {11, DescriptorType::SampledImage, depth_atlas_};
                            rp2[12] = {12, DescriptorType::SampledImage, normal_atlas_};
                            // A: tile-grid output + aux evidence + EMA history.
                            rp2[13] = {13, DescriptorType::StorageImage, indirect_tile_tex_[outIdx]};
                            rp2[14] = {14, DescriptorType::StorageImage, indirect_tile_aux_};
                            rp2[15] = {15, DescriptorType::SampledImage, indirect_tile_tex_[temporalPrevIdx]};
                            // [WRC 计划 Stream D2]: 卡片页表（过表寻址）。
                            rp2[16] = {16, DescriptorType::StorageBuffer, pageTableBuf};
                            // [SC per-mesh P4]: 射线 lookup 分类统计（原子累加）。
                            rp2[17] = {17, DescriptorType::StorageBuffer, ray_lookup_stats_buffer_};
                        } else {
                            rp2[0] = {0, DescriptorType::SampledImage,  tileQuerySource};
                            rp2[1] = {1, DescriptorType::StorageImage,  albedo_atlas_};
                            rp2[2] = {2, DescriptorType::SampledImage,  indirect_dummy_tex_};   // dummy (unused)
                            rp2[3] = {3, DescriptorType::StorageImage,  indirect_out_tex_[outIdx]};
                            rp2[4] = {0, DescriptorType::UniformBuffer, indirect_dummy_cb_};
                            rp2[5] = {1, DescriptorType::UniformBuffer, indirect_resolve_params_cb_[frameIdx]};
                            rp2[6] = {2, DescriptorType::StorageBuffer, card_generator_.GetCardLookupBuffer()};
                            rp2[7] = {3, DescriptorType::StorageBuffer, card_generator_.GetCardDataBuffer()};
                            rp2[8] = {4, DescriptorType::StorageBuffer, ray_hits_buffer_};
                            rp2[9] = {6, DescriptorType::SampledImage,  indirect_out_tex_[temporalPrevIdx]};   // T3.3 EMA history (N-1)
                            rp2[10] = {5, DescriptorType::StorageBuffer,   // [[buffer(5)]]
                                       indirect_tile_valid_buffer_ != handles::INVALID_RESOURCE
                                           ? indirect_tile_valid_buffer_
                                           : GetCardDispatchBuffer(frameIdx)};
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
                            rp2[15] = {11, DescriptorType::SampledImage, indirect_tile_tex_[temporalPrevIdx]};
                        }
                        if(geometry_first_requested_) {
                            rp2[18]={18,DescriptorType::StorageBuffer,ray_triangle_buffer_!=handles::INVALID_RESOURCE?ray_triangle_buffer_:ownerBuffer};
                            rp2[19]={19,DescriptorType::StorageBuffer,triangle_atlas_buffer_};
                            rp2[20]={20,DescriptorType::StorageBuffer,triangle_support_buffer_!=handles::INVALID_RESOURCE?triangle_support_buffer_:triangle_atlas_buffer_};
                        }
                        if(!boundary_consumer_requested_){
                        UpdateDescriptorSet(device_, indirect_resolve_set_[frameIdx], rp2, isVk?(geometry_first_requested_?21u:18u):16u);
                        cmd->BindComputePipeline(indirect_resolve_pipeline_);
                        const DescriptorSetHandle rsets[] = { indirect_resolve_set_[frameIdx] };
                        cmd->BindDescriptorSets(PipelineBindPoint::Compute, indirect_resolve_layout_, 0, 1, rsets, 0, nullptr);
                        DispatchScNativeLabel(device_,cmd,"SC.main.resolve.base",kTilesEff,kTilesEff,1);
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "SurfaceCache.IndirectResolve", "tile-radiance", indirect_tile_tex_[outIdx], false);
                        }
                        if(boundaryProofNow && BoundaryProof().radio_traced && isVk && geometry_first_requested_){
                            auto& p=BoundaryProof();std::vector<DescriptorData> descriptors(rp2,rp2+21);
                            descriptors[8].resource=p.radio_buffers[0];descriptors[18].resource=p.radio_buffers[1];
                            descriptors.push_back({21,DescriptorType::StorageBuffer,p.points});descriptors.push_back({22,DescriptorType::StorageBuffer,p.radio_buffers[3]});
                            descriptors.push_back({23,DescriptorType::StorageBuffer,p.radio_buffers[4]});
                            std::vector<DescriptorSetLayoutBinding> bindings;
                            for(const auto& d:descriptors)bindings.push_back({d.binding,d.type,1,ShaderStage::Compute,nullptr});
                            if(PrepareBoundaryRadiosityProof(3,bindings)){
                                UpdateDescriptorSet(device_,p.sets[3],descriptors.data(),u32(descriptors.size()));
                                cmd->MemoryBarrier(PipelineStage::ComputeShader|PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::ShaderWrite|AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
                                cmd->BindComputePipeline(p.pipelines[3]);cmd->BindDescriptorSets(PipelineBindPoint::Compute,p.layouts[3],0,1,&p.sets[3],0,nullptr);BoundaryTimestamp(cmd,6);DispatchScNativeLabel(device_,cmd,"SC.boundary.resolve",(p.count+63)/64,1,1);BoundaryTimestamp(cmd,7);
                                if(p.readback_requested){
                                const ResourceHandle sources[]={p.radio_buffers[0],p.radio_buffers[1],p.radio_buffers[2],p.radio_buffers[3],p.radio_buffers[4],indirect_trace_params_cb_[frameIdx],indirect_resolve_params_cb_[frameIdx],card_generator_.GetCardLookupBuffer()};
                                const u64 rays=u64(p.count)*32,sizes[]={rays*32,rays*4,rays*32,u64(p.count)*64,rays*16,sizeof(IndirectTraceParamsGPU),sizeof(IndirectResolveParamsGPU),u64(card_generator_.GetLookups().size())*sizeof(SurfaceCacheCardLookup)};
                                for(u32 i=0;i<8;++i){p.radio_offsets[i]=p.radio_bytes;p.radio_sizes[i]=sizes[i];p.radio_bytes+=(sizes[i]+15)&~u64(15);}
                                BufferDesc d{};d.size=p.radio_bytes;d.memoryUsage=d.usage=GPUMemoryUsage::Readback;if(p.radio_buffers[5]!=handles::INVALID_RESOURCE && p.radio_staging_capacity<p.radio_bytes){device_->DestroyBuffer(p.radio_buffers[5]);p.radio_buffers[5]=handles::INVALID_RESOURCE;}
                                if(p.radio_buffers[5]==handles::INVALID_RESOURCE){p.radio_buffers[5]=device_->CreateBuffer(d);p.radio_staging_capacity=p.radio_bytes;}
                                if(p.radio_buffers[5]==handles::INVALID_RESOURCE)p.failed=true;
                                else {cmd->MemoryBarrier(PipelineStage::ComputeShader|PipelineStage::Host,PipelineStage::Transfer,AccessFlag::ShaderWrite|AccessFlag::HostWrite,AccessFlag::TransferRead);
                                    for(u32 i=0;i<8;++i)if(sizes[i])cmd->CopyBuffer(sources[i],p.radio_buffers[5],0,p.radio_offsets[i],sizes[i]);
                                    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
                                    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::ComputeShader,AccessFlag::TransferRead,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);


                                }
                                }
                                if(!p.failed){p.radio_recorded=true;p.lookup_lighting=rp2[0].resource;
                                    if(!RecordBoundaryRadianceCache(cmd,params_cb_[frameIdx]))p.failed=true;}
                            }
                        }
                        if(boundary_consumer_requested_){
                            auto& p=BoundaryProof();
                            const bool ready=boundaryProofNow && !p.failed && p.cache_recorded &&
                                p.mixed_index!=handles::INVALID_RESOURCE && p.epoch==card_generator_.GetAllocationEpoch() &&
                                p.frame==current_frame_index && p.cache_key.geometry==geometry_epoch_;
                            std::vector<DescriptorData> consumer(rp2,rp2+21);
                            consumer.push_back({21,DescriptorType::StorageBuffer,ready?p.mixed_index:ownerBuffer});
                            consumer.push_back({22,DescriptorType::StorageBuffer,ready?p.points:ownerBuffer});
                            consumer.push_back({23,DescriptorType::StorageBuffer,ready?p.cache:ownerBuffer});
                            if(auto* params=static_cast<IndirectResolveParamsGPU*>(device_->MapBuffer(indirect_resolve_params_cb_[frameIdx]))){
                                if(ready)params->ray_lookup|=1024u;else params->ray_lookup&=~1024u;
                                device_->UnmapBuffer(indirect_resolve_params_cb_[frameIdx]);
                            }else {p.failed=true;PRIMAL_LOG_STREAM(Error,"Render")<<"[SC_BOUNDARY_CONSUMER] params update failed"<<std::endl;}
                            UpdateDescriptorSet(device_,indirect_resolve_set_[frameIdx],consumer.data(),u32(consumer.size()));
                            cmd->MemoryBarrier(PipelineStage::ComputeShader|PipelineStage::Transfer|PipelineStage::Host,PipelineStage::ComputeShader,
                                AccessFlag::ShaderWrite|AccessFlag::TransferRead|AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
                            cmd->BindComputePipeline(indirect_resolve_pipeline_);
                            cmd->BindDescriptorSets(PipelineBindPoint::Compute,indirect_resolve_layout_,0,1,&indirect_resolve_set_[frameIdx],0,nullptr);
                            if(ready)BoundaryTimestamp(cmd,10);DispatchScNativeLabel(device_,cmd,"SC.main.resolve.consumer",kTilesEff,kTilesEff,1);if(ready)BoundaryTimestamp(cmd,11);
                            p.consumer_recorded=ready;
                            if(ready)PRIMAL_LOG_STREAM(Info,"Render")<<"[SC_BOUNDARY_CONSUMER] main resolve frame="<<current_frame_index<<" slot="<<frameIdx
                                <<" completion="<<p.completion_serial<<" readback="<<p.readback_requested<<" reused="<<p.reused
                                <<" points="<<p.points<<" cache="<<p.cache<<" index="<<p.mixed_index<<std::endl;
                        }
                        // Readback is performed by ReadbackRayLookupStats only after the
                        // caller has submitted this frame. Never Map a recording dispatch.
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
                if (lumencompare::enabled) lumencompare::Output(device_, cmd, "SurfaceCache.IndirectReconstruct", "indirect-out", indirect_out_tex_[outIdx], false);
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
                // [2026-09-24] 曾以 Error 级每帧打印 → 编辑器日志红色刷屏；
                // 改 Info + 一次性。
                {
                    static bool s_lightEvalLogged = false;
                    if (!s_lightEvalLogged) {
                        s_lightEvalLogged = true;
                        PRIMAL_LOG_STREAM(Info, "Render") << "[SurfaceCache] LightEval dispatch: " << total_texels
                                  << " texels, " << card_count << " cards, "
                                  << groups << " groups" << std::endl;
                    }
                }

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


#include "BoundaryLightingProof.inl"

} // namespace primal::graphics::lumen
