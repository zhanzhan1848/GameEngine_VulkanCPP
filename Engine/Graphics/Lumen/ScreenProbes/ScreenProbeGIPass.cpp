#include "ScreenProbeGIPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Engine/Graphics/Dawn/ShaderLoader.h"  // for dawn::LoadWGSL
// MetalDevice.h intentionally excluded — causes Rect naming conflict with MacTypes.h
#include "Graphics/Nanite/GlobalSDF.h"
#include "Graphics/Field/FieldRegistry.h"
#include "Graphics/Field/FieldView.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <set>
#include <algorithm>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::lumen {

using namespace rhi;
using namespace rendergraph;

namespace {

// ============================================================================
// Descriptor update helper
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
                   params[i].type == DescriptorType::StorageImage ||
                   params[i].type == DescriptorType::Sampler ||
                   params[i].type == DescriptorType::SampledDepthImage) {
            // 2026-09-05：补 SampledDepthImage——此前 Vulkan denoise 的深度
            // 绑定（binding 2）不写 imageInfo，描述符为空 → 深度永远读 0 →
            // 全像素走天空早退 → filtered 输出全零（fusion 的 SPGI 贡献
            // 实际为黑）。Metal 走隐式描述符不受影响。
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
// ============================================================================

static const std::string SHADER_BASE_DIR =
    utils::ShaderRegistry::ResolveShaderPath("Engine/Graphics/Metal/shaders/") + "/";
static const std::string SCREEN_PROBE_SHADER_DIR = SHADER_BASE_DIR + "Lumen/";

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

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

    if (platform == rhi::RHIPlatform::Dawn) {
        // WGSL path. dawn::LoadWGSL has two overloads (ShaderLoader.h):
        //   WASM:   LoadWGSL(const char* name) — kShaderMap + MEMFS fallback at
        //           /Engine/Graphics/Dawn/shaders/{name}.wgsl
        //   Native: LoadWGSL(const std::string& path) — opens filesystem path as-is
        // ScreenProbe shaders live in Engine/Graphics/Dawn/shaders/Lumen/. On
        // native we pass the full path. On WASM, Emscripten --embed-file
        // preserves subdirs, so the shader is at
        // /Engine/Graphics/Dawn/shaders/Lumen/ in MEMFS — pass a relative name
        // and let LoadWGSL's fallback construct the right path.
        std::string src;
#ifdef __EMSCRIPTEN__
        std::string lumenName = std::string("Lumen/") + shaderName;
        src = dawn::LoadWGSL(lumenName.c_str());
#else
        std::string path = std::string("Engine/Graphics/Dawn/shaders/Lumen/") + shaderName + ".wgsl";
        src = dawn::LoadWGSL(path);
#endif
        if (src.empty()) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ScreenProbeGI] Failed to load WGSL shader: " << shaderName << std::endl;
            return {};
        }
        return std::vector<u8>(src.begin(), src.end());
    }

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
        PRIMAL_LOG_STREAM(Error, "Render") << "[ScreenProbeGI] Failed to load SPIR-V: " << shaderName << std::endl;
        return {};
    }

    // Metal path: load .metal source + resolve #includes
    std::string shaderPath = SCREEN_PROBE_SHADER_DIR + shaderName + ".metal";
    std::string source = ReadFileToString(shaderPath);

    // T8.13 SPGI parity track: PRIMAL_SPGI_PARITY=1 prefers <name>Parity.metal
    // — faithful Metal ports of the Vulkan ScreenProbe*.comp kernels — per
    // stage, falling back to the engine shader whenever the port doesn't
    // exist yet. Mirrors the LumenDDGIPass PRIMAL_DDGI_PARITY mechanism.
    // WARNING: the SPGI chain still has an unresolved frame-2 GPU fault; run
    // only with Metal validation layers until fixed.
    if (std::getenv("PRIMAL_SPGI_PARITY")) {
        const std::string parityPath = SCREEN_PROBE_SHADER_DIR + shaderName + "Parity.metal";
        std::string paritySource = ReadFileToString(parityPath);
        if (!paritySource.empty()) {
            shaderPath = parityPath;
            source = std::move(paritySource);
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[ScreenProbeGI] Parity port in effect: " << shaderName << "Parity.metal" << std::endl;
        }
    }

    if (source.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ScreenProbeGI] Failed to load shader: " << shaderName << std::endl;
        return {};
    }

    std::set<std::string> included;
    std::string resolved = ResolveIncludes(source, SCREEN_PROBE_SHADER_DIR, included);

    return std::vector<u8>(resolved.begin(), resolved.end());
}

// ============================================================================
// Per-pass data struct for the RenderGraph pass
// ============================================================================

struct ScreenProbePassData {
    RGResourceHandle gbuffer_depth;
    RGResourceHandle gbuffer_normal;
    RGResourceHandle prev_frame_color;
    RGResourceHandle gi_output;
    // T3.5: cascade textures of this frame (write = EMA target, prev = EMA
    // history consumed by the cascade update AND the finalize far-cache).
    // SH-L1: [0] = L0, [1..3] = L1x/L1y/L1z coefficient volumes.
    RGResourceHandle cascade_write[4];
    RGResourceHandle cascade_prev[4];
};

} // anonymous namespace

// ============================================================================
// ScreenProbeGIPass Implementation
// ============================================================================

ScreenProbeGIPass::~ScreenProbeGIPass() {
    Shutdown();
}

// F3 (M7): 时域策略已实装 — EMA=现有路径（默认，行为不变）；
// EMAVarianceClamp=+邻域 AABB 方差钳制；Reprojection=纯双线性历史复用。
void ScreenProbeGIPass::SetTemporalStrategy(GITemporalStrategy strategy) {
    if (temporal_strategy_ == strategy) return;
    temporal_strategy_ = strategy;
    PRIMAL_LOG_STREAM(Info, "Render") << "[ScreenProbeGI] Temporal strategy -> "
              << GITemporalStrategyName(strategy)
              << " (EMA=0 / EMAVarianceClamp=1 / Reprojection=2, effective next frame)"
              << std::endl;
}

bool ScreenProbeGIPass::Initialize(RHIDeviceBase* device,
                                    u32 render_width, u32 render_height,
                                    const ScreenProbeParams& params) {
    if (initialized_) return true;

    if (!device || params.downsample_factor == 0 ||
        params.rays_per_probe == 0 || params.rays_per_probe > 64) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[ScreenProbeGI] Requires a device, nonzero downsample and 1..64 rays/probe" << std::endl;
        return false;
    }

    device_ = device;
    params_ = params;
    render_width_ = render_width;
    render_height_ = render_height;

    // Compute probe grid dimensions
    grid_width_ = (render_width + params_.downsample_factor - 1) / params_.downsample_factor;
    grid_height_ = (render_height + params_.downsample_factor - 1) / params_.downsample_factor;

    CreateDescriptorSetLayouts();
    CreatePipelines();
    CreateConstantBuffers();
    CreateBuffers();
    CreateRadianceCascade();

    initialized_ = true;

    u32 totalProbes = grid_width_ * grid_height_;
    PRIMAL_LOG_STREAM(Info, "Render") << "[ScreenProbeGI] Initialized (grid: "
              << grid_width_ << "x" << grid_height_
              << " = " << totalProbes << " probes, "
              << params_.rays_per_probe << " rays/probe, "
              << "downsample=" << params_.downsample_factor << ")" << std::endl;
    return true;
}

void ScreenProbeGIPass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    device_ = nullptr;
}

// ============================================================================
// Private helper methods
// ============================================================================

void ScreenProbeGIPass::CreateDescriptorSetLayouts() {
    const bool isVk = device_->GetPlatform() == rhi::RHIPlatform::Vulkan;

    // --- Place: 2 textures (depth, normal) + 2 SSBO (positions, normals) + 1 UBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depthTexture
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normalTexture
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probePositions (write)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probeNormals (write)
            {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // ScreenProbeGlobalData
        };
        place_set_layout_ = device_->CreateDescriptorSetLayout({5, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            // Buffers
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
        };
        DescriptorSetLayoutDesc layoutDesc{5, bindings};
        place_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Trace: 5 textures (3 SDF + prev_color + surface_cache_lighting) + 3 SSBO (positions, normals, radiance) + 2 SSBO (card_data, card_lookup) + 2 SSBO (card grid, T1.3) + 1 UBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf0 (3D)
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf1 (3D)
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf2 (3D)
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prevFrameColor
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // surface_cache_lighting
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probePositions (readonly)
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probeNormals (readonly)
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probeRadiance (read-write)
            {8, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // ScreenProbeGlobalData
            {9, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards (readonly)
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // cardLookup (readonly)
            {11, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // sc_grid_offsets (T1.3)
            {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // sc_grid_cards (T1.3)
            {13, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // ddgi_irradiance (T2.2)
            {14, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // importanceSH (F2)
        };
        // Mark SDF textures as 3D view dimension
        bindings[0].is3D = true;
        bindings[1].is3D = true;
        bindings[2].is3D = true;
        // Mark read-only SSBOs
        bindings[5].readonly = true;  // probePositions
        bindings[6].readonly = true;  // probeNormals
        bindings[9].readonly = true;  // cards
        bindings[10].readonly = true; // cardLookup
        bindings[11].readonly = true; // sc_grid_offsets
        bindings[12].readonly = true; // sc_grid_cards
        bindings[13].readonly = true; // ddgi_irradiance
        bindings[14].readonly = true; // importanceSH (F2)
        trace_set_layout_ = device_->CreateDescriptorSetLayout({15, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures (SDF cascades + prev frame color + surface cache lighting atlas)
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 2
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev frame color
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // surface cache lighting atlas
            // Buffers
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals (read)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe radiance (write)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // surface cache card data
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // surface cache card lookup
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc_grid_offsets (T1.3)
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc_grid_cards (T1.3)
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ddgi_irradiance (T2.2)
            {9, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // importanceSH (F2)
        };
        DescriptorSetLayoutDesc layoutDesc{15, bindings};
        trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- T5.1 2a: HZB near-field trace — 1 texture (hzb mip chain) + 2 SSBO (positions, normals) + 1 SSBO (hit out) + 1 UBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // hzb_texture (mip chain)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_buffer (out)
            {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // importanceSH (F2)
        };
        bindings[1].readonly = true;
        bindings[2].readonly = true;
        bindings[5].readonly = true;  // importanceSH (F2)
        hzb_trace_set_layout_ = device_->CreateDescriptorSetLayout({6, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // hzb_texture
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_buffer (out)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // importanceSH (F2)
        };
        hzb_trace_set_layout_ = device_->CreateDescriptorSetLayout({6, bindings});
    }

    // --- T5.1 2b: SDF far-field trace — 3 textures (SDF) + 2 SSBO (positions, normals) + 1 SSBO (hit_distance rw) + 1 UBO ---
    // Vulkan: separate SPIR-V files; Metal: entries in ScreenProbeTraceRays.metal.
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // sdf2
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_buffer (rw)
            {6, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // importanceSH (F2)
            // T4.3 二批: detail slot table + 4 textures (distance refinement)
            {8, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // detail slot table
            {9,  DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // detail slot 0
            {10, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // detail slot 1
            {11, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // detail slot 2
            {12, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // detail slot 3
        };
        bindings[0].is3D = true;
        bindings[1].is3D = true;
        bindings[2].is3D = true;
        bindings[3].readonly = true;
        bindings[4].readonly = true;
        bindings[7].readonly = true;  // importanceSH (F2)
        for (u32 bi = 9; bi <= 12; ++bi) {
            bindings[bi].is3D = true;
            bindings[bi].unfilterableFloat = true;
        }
        sdf_trace_set_layout_ = device_->CreateDescriptorSetLayout({13, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 0
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 1
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SDF 2
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_buffer (rw)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // importanceSH (F2)
            // T4.3 二批 (Metal overlapping namespaces): buffer(5) table + texture(3..6)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // detail slot table
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // detail slot 0
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // detail slot 1
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // detail slot 2
            {6, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // detail slot 3
        };
        DescriptorSetLayoutDesc layoutDesc{13, bindings};
        sdf_trace_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- T5.1 2c: Finalize — 2 textures (prev_color + sc_lighting) + radiance chain buffers ---
    // Vulkan flat 0..16; Metal overlapping (texture 0-5, buffer 0-10).
    // T3.5 + SH-L1: bindings 13-16 / textures (2)-(5) = radiance cascade
    // L0+L1xyz (prev frame) for the far-cache fast path (4 texel reads vs the
    // 16-read tetrahedral query).
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prevFrameColor
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // surface_cache_lighting
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_buffer (in)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe radiance (out)
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cards
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // cardLookup
            {9, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc_grid_offsets (T1.3)
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // sc_grid_cards (T1.3)
            {11, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // ddgi_irradiance (T2.2)
            {12, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // importanceSH (F2)
            {13, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // radiance cascade L0 (T3.5)
            {14, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // radiance cascade L1x (SH-L1)
            {15, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // radiance cascade L1y (SH-L1)
            {16, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // radiance cascade L1z (SH-L1)
            {17, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr},  // SC depth for hit validation
            {18, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // P1: trace 分支统计（diag 门控）
        };
        bindings[2].readonly = true;
        bindings[3].readonly = true;
        bindings[4].readonly = true;
        bindings[7].readonly = true;
        bindings[8].readonly = true;
        bindings[9].readonly = true;
        bindings[10].readonly = true;
        bindings[11].readonly = true;
        bindings[12].readonly = true; // importanceSH (F2)
        bindings[13].is3D = true;     // radiance cascade (T3.5)
        bindings[14].is3D = true;     // radiance cascade (SH-L1)
        bindings[15].is3D = true;     // radiance cascade (SH-L1)
        bindings[16].is3D = true;     // radiance cascade (SH-L1)
        finalize_set_layout_ = device_->CreateDescriptorSetLayout({19, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // prev frame color
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // surface cache lighting atlas
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // radiance cascade L0 (T3.5)
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // radiance cascade L1x (SH-L1)
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // radiance cascade L1y (SH-L1)
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // radiance cascade L1z (SH-L1)
            {6, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // SC depth
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // hit_buffer (in)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe radiance (out)
            {5, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // surface cache card data
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // surface cache card lookup
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc_grid_offsets (T1.3)
            {8, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // sc_grid_cards (T1.3)
            {9, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ddgi_irradiance (T2.2)
            {10, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr}, // importanceSH (F2)
        };
        for (int b = 2; b < 6; ++b) bindings[b].is3D = true;  // radiance cascade (T3.5 + SH-L1)
        DescriptorSetLayoutDesc layoutDesc{18, bindings};
        finalize_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Gather: 2 textures (depth, normal) + 1 storage image (output) + 2 SSBO + 1 UBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depthTexture
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normalTexture
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // outputTexture (rgba16f)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probePositions (readonly)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probeSH (readonly)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // ScreenProbeGlobalData
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probeNormals (readonly)
        };
        // Mark read-only SSBOs
        bindings[3].readonly = true;  // probePositions
        bindings[4].readonly = true;  // probeSH
        bindings[6].readonly = true;  // probeNormals
        gather_set_layout_ = device_->CreateDescriptorSetLayout({7, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            // Textures
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // output texture
            // Buffers
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions (read)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe SH (read, filtered)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // global data
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals (read)
        };
        DescriptorSetLayoutDesc layoutDesc{7, bindings};
        gather_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Average: 3 SSBO (rayRadiance in, probeSH out, probeNormals in) + 1 UBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // rayRadiance (readonly)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probeSHOut (read-write)
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // {totalProbes, raysPerProbe} (16 bytes)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probeNormals (readonly)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // importanceSH (F2)
        };
        // Mark read-only SSBOs
        bindings[0].readonly = true;  // rayRadiance
        bindings[3].readonly = true;  // probeNormals
        bindings[4].readonly = true;  // importanceSH (F2)
        avg_set_layout_ = device_->CreateDescriptorSetLayout({5, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ray radiance (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe SH output (write)
            {2, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // totalProbes + raysPerProbe
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe normals (read, for ray direction)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // importanceSH (F2)
        };
        DescriptorSetLayoutDesc layoutDesc{5, bindings};
        avg_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Temporal: 7 SSBO + 1 UBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // currentSH (readonly)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // historySH (readonly)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // currentPositions (readonly)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // historyPositions (readonly)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // outputSH (read-write)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // TemporalConstants
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // currentNormals (readonly)
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // historyNormals (readonly)
        };
        // Mark read-only SSBOs
        bindings[0].readonly = true;  // currentSH
        bindings[1].readonly = true;  // historySH
        bindings[2].readonly = true;  // currentPositions
        bindings[3].readonly = true;  // historyPositions
        bindings[6].readonly = true;  // currentNormals
        bindings[7].readonly = true;  // historyNormals
        temporal_set_layout_ = device_->CreateDescriptorSetLayout({8, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // current avg radiance (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // history avg radiance (read)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // current positions (read)
            {3, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // history positions (read)
            {4, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // output radiance (write)
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // temporal constants
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // current normals (read)
            {7, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // history normals (read)
        };
        DescriptorSetLayoutDesc layoutDesc{8, bindings};
        temporal_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Spatial Filter: 3 SSBO (inputSH, outputSH, positions) + 1 UBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // inputSH (readonly)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // outputSH (read-write)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probePos (readonly)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // {totalProbes, gridW, sigma, pad}
        };
        // Mark read-only SSBOs
        bindings[0].readonly = true;  // inputSH
        bindings[2].readonly = true;  // probePos
        spatial_set_layout_ = device_->CreateDescriptorSetLayout({4, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // input SH (read)
            {1, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // output SH (write)
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // probe positions (read)
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // spatial constants
        };
        DescriptorSetLayoutDesc layoutDesc{4, bindings};
        spatial_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }

    // --- Denoise: 3 sampled textures + 1 storage image + 1 UBO ---
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // giInput
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normalTexture
            {2, DescriptorType::SampledDepthImage, 1, ShaderStage::Compute, nullptr},  // depthTexture (texelFetch)
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // giOutput (rgba16f)
            {4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // DenoiseParams (32 bytes)
        };
        denoise_set_layout_ = device_->CreateDescriptorSetLayout({5, bindings});
    } else {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // GI input (from Gather)
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // normal
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // depth
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // GI output (filtered)
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // denoise params
        };
        DescriptorSetLayoutDesc layoutDesc{5, bindings};
        denoise_set_layout_ = device_->CreateDescriptorSetLayout(layoutDesc);
    }
}

void ScreenProbeGIPass::CreatePipelines() {
    auto CompileShader = [&](const char* name, const char* entry) -> ShaderHandle {
        auto code = LoadShaderBytecode(device_, name);
        if (code.empty()) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ScreenProbeGI] Failed to load shader source: " << name << std::endl;
            return handles::INVALID_SHADER;
        }
        // Bring-up aid: MetalShader's compile-error log carries no shader name,
        // so print one line before CreateShader to attribute failures.
        PRIMAL_LOG_STREAM(Info, "Render") << "[ScreenProbeGI] compiling " << name << " : " << entry << std::endl;
        return device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    };

    auto placeShader = CompileShader("ScreenProbePlace", "screen_probe_place");
    auto traceShader = CompileShader("ScreenProbeTraceRays", "screen_probe_trace_rays");
    auto avgShader = CompileShader("ScreenProbeAverage", "screen_probe_average");
    if (avgShader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ScreenProbeGI] Average shader compilation failed" << std::endl;
        return;
    }
    auto gatherShader = CompileShader("ScreenProbeGather", "screen_probe_gather");
    auto temporalShader = CompileShader("ScreenProbeTemporal", "screen_probe_temporal");
    auto spatialShader = CompileShader("ScreenProbeSpatialFilter", "screen_probe_spatial_filter");
    auto denoiseShader = CompileShader("ScreenProbeDenoise", "screen_probe_denoise");

    // T5.1 hybrid split trace — HZB near (2a) + SDF far (2b) + finalize (2c).
    // Vulkan: separate SPIR-V files per kernel; Metal: entries in
    // ScreenProbeTraceRays.metal (single source file, multiple kernels).
    const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
    ShaderHandle hzbTraceShader = handles::INVALID_SHADER;
    ShaderHandle sdfTraceShader = handles::INVALID_SHADER;
    ShaderHandle finalizeShader = handles::INVALID_SHADER;
    if (isVk) {
        hzbTraceShader = CompileShader("ScreenProbeTraceHZB", "screen_probe_trace_hzb");
        sdfTraceShader = CompileShader("ScreenProbeTraceSDF", "screen_probe_trace_sdf");
        finalizeShader = CompileShader("ScreenProbeTraceFinalize", "screen_probe_trace_finalize");
    } else {
        hzbTraceShader = CompileShader("ScreenProbeTraceRays", "screen_probe_trace_hzb");
        sdfTraceShader = CompileShader("ScreenProbeTraceRays", "screen_probe_trace_sdf");
        finalizeShader = CompileShader("ScreenProbeTraceRays", "screen_probe_trace_finalize");
    }

    if (placeShader == handles::INVALID_SHADER ||
        traceShader == handles::INVALID_SHADER ||
        gatherShader == handles::INVALID_SHADER ||
        temporalShader == handles::INVALID_SHADER ||
        spatialShader == handles::INVALID_SHADER ||
        denoiseShader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ScreenProbeGI] Shader compilation failed" << std::endl;
        return;
    }
    // Split shaders are optional — missing ones route through the monolith.
    if (sdfTraceShader == handles::INVALID_SHADER || finalizeShader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ScreenProbeGI] Split-trace shaders unavailable — "
                     "falling back to monolithic trace" << std::endl;
    }

    // Create pipeline layouts
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &place_set_layout_;
        place_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &trace_set_layout_;
        trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    if (hzb_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &hzb_trace_set_layout_;
        hzb_trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    if (sdf_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &sdf_trace_set_layout_;
        sdf_trace_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    if (finalize_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &finalize_set_layout_;
        finalize_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &avg_set_layout_;
        avg_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &gather_set_layout_;
        gather_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &temporal_set_layout_;
        temporal_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &spatial_set_layout_;
        spatial_layout_ = device_->CreatePipelineLayout(plDesc);
    }
    {
        PipelineLayoutDesc plDesc;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &denoise_set_layout_;
        denoise_layout_ = device_->CreatePipelineLayout(plDesc);
    }

    // Create compute pipelines
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = placeShader;
        pipeDesc.layout = place_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        place_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = traceShader;
        pipeDesc.layout = trace_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    // T5.1 hybrid split: HZB near (2a) + SDF far (2b) + finalize (2c).
    // Each stays within one read category per kernel (texture2D / texture3D /
    // buffers) — the Apple Silicon split-dispatch constraint.
    if (hzbTraceShader != handles::INVALID_SHADER && hzb_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = hzbTraceShader;
        pipeDesc.layout = hzb_trace_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        hzb_trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    if (sdfTraceShader != handles::INVALID_SHADER && sdf_trace_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = sdfTraceShader;
        pipeDesc.layout = sdf_trace_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        sdf_trace_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    if (finalizeShader != handles::INVALID_SHADER && finalize_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = finalizeShader;
        pipeDesc.layout = finalize_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        finalize_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = avgShader;
        pipeDesc.layout = avg_layout_;
        // All 64 lanes initialize shared reduction storage, including inactive rays.
        pipeDesc.threadGroupSize = {64, 1, 1};
        avg_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = gatherShader;
        pipeDesc.layout = gather_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};  // 2D for threadgroup probe caching
        gather_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = temporalShader;
        pipeDesc.layout = temporal_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        temporal_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = spatialShader;
        pipeDesc.layout = spatial_layout_;
        pipeDesc.threadGroupSize = {64, 1, 1};
        spatial_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
    {
        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = denoiseShader;
        pipeDesc.layout = denoise_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};  // 2D dispatch for screen-space filter
        denoise_pipeline_ = device_->CreateComputePipeline(pipeDesc);

        // 诊断：报告所有 SPGI pipeline 有效性
        {
            auto check = [](const char* name, rhi::PipelineHandle h) {
                if (h == rhi::handles::INVALID_PIPELINE)
                    PRIMAL_LOG_STREAM(Error, "Render") << "[SPGI] PIPELINE INVALID: " << name << std::endl;
            };
            check("place", place_pipeline_);
            check("trace_rays", trace_pipeline_);
            check("trace_hzb", hzb_trace_pipeline_);
            check("trace_sdf", sdf_trace_pipeline_);
            check("finalize", finalize_pipeline_);
            check("average", avg_pipeline_);
            check("gather", gather_pipeline_);
            check("temporal", temporal_pipeline_);
            check("spatial", spatial_pipeline_);
            check("denoise", denoise_pipeline_);
        }
    }

    // Create triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        {
            DescriptorSetDesc dsDesc{place_set_layout_};
            place_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{trace_set_layout_};
            trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (hzb_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{hzb_trace_set_layout_};
            hzb_trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (sdf_trace_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{sdf_trace_set_layout_};
            sdf_trace_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        if (finalize_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            DescriptorSetDesc dsDesc{finalize_set_layout_};
            finalize_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{avg_set_layout_};
            avg_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{gather_set_layout_};
            gather_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{temporal_set_layout_};
            temporal_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{spatial_set_layout_};
            spatial_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
        {
            DescriptorSetDesc dsDesc{denoise_set_layout_};
            denoise_ds_[i] = device_->CreateDescriptorSet(dsDesc);
        }
    }
}

void ScreenProbeGIPass::CreateConstantBuffers() {
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = 640;  // ScreenProbeGlobalData padded (expanded for surface_cache_params)
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        global_cb_[i] = device_->CreateBuffer(desc);
    }
    // T4.3 二批: detail-SDF slot table (16B header + 4 × 160B slots = 656B).
    // Always bound; header count=0 keeps the shader inert when the feature
    // is off (PRIMAL_DDGI_DETAIL_SDF unset / no nearby per-mesh SDF).
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = 656;
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        detail_table_cb_[i] = device_->CreateBuffer(desc);
    }
}

// SPGI bring-up bisect (TEMP): PRIMAL_SPGI_SKIP="trace,hzb,temporal,spatial,denoise,post,gather"
// — comma/substring list of dispatch groups to skip, so a device hang or
// visual artifact can be attributed to a single stage in one run. `hzb`
// skips the near-field HZB dispatch and clears its UBO mip-level gate so SDF
// owns every ray. `temporal` and `denoise` skip only those filters while
// preserving the remaining post stages. `spatial`
// skips only the probe-grid SpatialFilter; `post` keeps its historical meaning
// and skips Temporal, SpatialFilter, and Denoise together. Used together with
// PRIMAL_SPGI=1.
static bool SpgiStageSkipped(const char* stage) {
    static const char* skipList = std::getenv("PRIMAL_SPGI_SKIP");
    if (!skipList) return false;
    return std::strstr(skipList, stage) != nullptr;
}

// T3.5: radiance cascade kill switch. PRIMAL_GI_CASCADE=0 restores the legacy
// 16-read tetrahedral far-cache query (dispatch + finalize fast path both off).
static bool CascadeEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("PRIMAL_GI_CASCADE");
        return !(e && std::strcmp(e, "0") == 0);
    }();
    return enabled;
}
static void SpgiStageMark(u32 frameIdx, const char* stage) {
    static u32 marks = 0;
    if (marks < 30) {  // bring-up: first frames only
        ++marks;
        PRIMAL_LOG_STREAM(Info, "Render") << "[SPGI-MARK] f" << frameIdx << " " << stage << " encoded (CPU)" << std::endl;
    }
}

void ScreenProbeGIPass::CreateBuffers() {
    u32 totalProbes = grid_width_ * grid_height_;

    // Probe positions buffers (triple-buffered): float4 per probe
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = (u64)totalProbes * sizeof(float) * 4;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes;
        desc.structured.elementStride = sizeof(float) * 4;
        probe_positions_buffer_[i] = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(probe_positions_buffer_[i]);
        if (mapped) {
            memset(mapped, 0, desc.size);
            device_->UnmapBuffer(probe_positions_buffer_[i]);
        }
    }

    // Probe normals buffers (triple-buffered): float4 per probe
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = (u64)totalProbes * sizeof(float) * 4;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes;
        desc.structured.elementStride = sizeof(float) * 4;
        probe_normals_buffer_[i] = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(probe_normals_buffer_[i]);
        if (mapped) {
            memset(mapped, 0, desc.size);
            device_->UnmapBuffer(probe_normals_buffer_[i]);
        }
    }

    // Probe radiance buffers (triple-buffered): float4 per ray
    for (int i = 0; i < 3; ++i) {
        u64 radianceSize = (u64)totalProbes * params_.rays_per_probe * sizeof(float) * 4;
        BufferDesc desc{};
        desc.size = radianceSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * params_.rays_per_probe;
        desc.structured.elementStride = sizeof(float) * 4;
        probe_radiance_buffer_[i] = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(probe_radiance_buffer_[i]);
        if (mapped) {
            memset(mapped, 0, radianceSize);
            device_->UnmapBuffer(probe_radiance_buffer_[i]);
        }
    }

    // Hit buffer (single, GPU-only intermediate for the T5.1 hybrid split):
    // vec4 per ray — x = hit distance, yz = HZB hit UV, w = source flag
    // (0 miss, 1 HZB near, 2 SDF far, 3 far-cache early-out [F1]).
    {
        u64 hitDistSize = (u64)totalProbes * params_.rays_per_probe * sizeof(float) * 4;
        BufferDesc desc{};
        desc.size = hitDistSize;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * params_.rays_per_probe;
        desc.structured.elementStride = sizeof(float) * 4;
        hit_distance_buffer_ = device_->CreateBuffer(desc);
    }

    // Per-probe SH coefficient buffers (triple-buffered, 4 float4 per probe for L0+L1)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = (u64)totalProbes * 4 * sizeof(float) * 4;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * 4;
        desc.structured.elementStride = sizeof(float) * 4;
        probe_avg_radiance_[i] = device_->CreateBuffer(desc);
        probe_temporal_radiance_[i] = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(probe_avg_radiance_[i]);
        if (mapped) {
            memset(mapped, 0, desc.size);
            device_->UnmapBuffer(probe_avg_radiance_[i]);
        }
    }

    // Average pass constant buffers (triple-buffered, 2 x uint32)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = 16;  // 2 x uint32, padded to 16 bytes
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        avg_constants_[i] = device_->CreateBuffer(desc);

        struct AvgCB { u32 totalProbes; u32 raysPerProbe; float frameSeed; float isMode; };
        auto* mapped = static_cast<AvgCB*>(device_->MapBuffer(avg_constants_[i]));
        if (mapped) {
            mapped->totalProbes = totalProbes;
            mapped->raysPerProbe = params_.rays_per_probe;
            mapped->frameSeed = -1.0f;
            mapped->isMode = params_.enable_importance ? 1.0f : 0.0f;
            device_->UnmapBuffer(avg_constants_[i]);
        }
    }

    // Temporal accumulation constant buffers (triple-buffered)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = 112;  // F3: prevVP (64B) + 10 scalars (mode + pad appended)
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        temporal_cb_[i] = device_->CreateBuffer(desc);
    }

    // Spatial filter constant buffers (triple-buffered)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = 16;  // totalProbes(u32) + gridW(u32) + sigma(f32) + pad(f32)
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        spatial_cb_[i] = device_->CreateBuffer(desc);
    }

    // Denoise constant buffers (triple-buffered)
    for (int i = 0; i < 3; ++i) {
        BufferDesc desc{};
        desc.size = 32;  // sigma_depth(f32) + sigma_normal(f32) + sigma_spatial(f32) + kernel_radius(u32) + width(u32) + height(u32) + pad(u32) + pad(u32)
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        denoise_cb_[i] = device_->CreateBuffer(desc);
    }

    // Output texture: full-resolution RGBA16_Float (raw Gather output)
    {
        TextureDesc desc{};
        desc.size = {render_width_, render_height_, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        output_texture_ = device_->CreateTexture(desc);
        device_->PrimeTextureLayout(output_texture_);  // P0-B 首帧 priming
    }

    // Filtered output texture: full-resolution RGBA16_Float (Denoise output)
    {
        TextureDesc desc{};
        desc.size = {render_width_, render_height_, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        output_texture_filtered_ = device_->CreateTexture(desc);
        device_->PrimeTextureLayout(output_texture_filtered_);  // P0-B 首帧 priming
    }

    // Dummy buffer for Metal validation when surface cache is unavailable
    {
        BufferDesc desc{};
        desc.size = 64;
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = 4;
        desc.structured.elementStride = 16;
        dummy_buffer_ = device_->CreateBuffer(desc);

        void* mapped = device_->MapBuffer(dummy_buffer_);
        if (mapped) {
            memset(mapped, 0, desc.size);
            device_->UnmapBuffer(dummy_buffer_);
        }
    }

    // Dummy texture2D for Metal validation when surface cache is unavailable
    {
        TextureDesc desc{};
        desc.size = {1, 1, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource;
        dummy_texture_2d_ = device_->CreateTexture(desc);
        device_->PrimeTextureLayout(dummy_texture_2d_);  // P0-B 首帧 priming
    }

    // P1（sponza-gap-reaudit §4.1/4.2）: finalize 分支统计缓冲。仅 Vulkan
    // （trace split/finalize 是 Vulkan-only 链）。host-visible 便于测试直接
    // 读回；布局必须始终含 binding 18（shader 静态使用），atomic 写由
    // diag_params.x 门控，默认路径只多绑 2KB 空缓冲。
    if (device_->GetPlatform() == RHIPlatform::Vulkan) {
        BufferDesc desc{};
        desc.size = 16 * 4 * 17;   // uvec4[branch][bucket] — see shader
        desc.type = BufferType::Structured;
        desc.usage = GPUMemoryUsage::Readback;
        desc.memoryUsage = GPUMemoryUsage::Readback;
        desc.structured.elementCount = 4 * 17;
        desc.structured.elementStride = 16;
        trace_stats_buffer_ = device_->CreateBuffer(desc);
        if (trace_stats_buffer_ != handles::INVALID_RESOURCE) {
            void* mapped = device_->MapBuffer(trace_stats_buffer_);
            if (mapped) {
                memset(mapped, 0, desc.size);
                device_->UnmapBuffer(trace_stats_buffer_);
            }
        }
    }
}

// ============================================================================
// T3.5: world-space radiance cascade
// ============================================================================

void ScreenProbeGIPass::CreateRadianceCascade() {
    const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;

    // 2 × 32³ RGBA16F double-buffered cascade volumes (L0 + 3 × L1 per slot
    // since the SH-L1 upgrade). Extent/origin derive from the DDGI volume per
    // dispatch (SetDDGIFallbackData may arrive after Initialize), so only the
    // fixed-resolution resources are created here.
    for (int i = 0; i < 2; ++i) {
        TextureDesc desc{};
        desc.size = {kRadianceCascadeRes, kRadianceCascadeRes, kRadianceCascadeRes};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture3D;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
        radiance_cascade_[i] = device_->CreateTexture(desc);
        device_->PrimeTextureLayout(radiance_cascade_[i]);  // P0-B 首帧 priming
        for (int c = 0; c < 3; ++c) {
            radiance_cascade_l1_[i][c] = device_->CreateTexture(desc);
            device_->PrimeTextureLayout(radiance_cascade_l1_[i][c]);  // P0-B 首帧 priming
        }
    }

    // 1³ dummy for the finalize cascade slot when the system is off.
    {
        TextureDesc desc{};
        desc.size = {1, 1, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::Texture3D;
        desc.usage = TextureUsage::ShaderResource;
        dummy_texture_3d_ = device_->CreateTexture(desc);
        // 3D dummy 不 Prime（3D 无 SampledImage 首帧绑定问题，且保持无状态）
    }

    // Update pipeline. Vulkan loads the precompiled .comp.spv (the standalone
    // build_spv block keeps the unnamed "main" entry); Metal compiles the
    // .metal source at runtime with the camelCase entry.
    const char* entry = isVk ? "main" : "radianceCascadeUpdate";
    auto code = LoadShaderBytecode(device_, "RadianceCascadeUpdate");
    if (code.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[ScreenProbeGI] Failed to load RadianceCascadeUpdate shader" << std::endl;
        return;
    }
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[ScreenProbeGI] compiling RadianceCascadeUpdate : " << entry << std::endl;
    auto shader = device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, entry);
    if (shader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[ScreenProbeGI] RadianceCascadeUpdate shader compilation failed" << std::endl;
        return;
    }

    // Descriptor layout — 10 bindings: cascade out L0+L1xyz (storage images) /
    // cascade prev L0+L1xyz (sampled 3D) / params UBO / DDGI irradiance
    // (readonly SSBO).
    if (isVk) {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // cascade out L0
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // cascade out L1x
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // cascade out L1y
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // cascade out L1z
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // cascade prev L0
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // cascade prev L1x
            {6, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // cascade prev L1y
            {7, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // cascade prev L1z
            {8, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // RadianceCascadeParams
            {9, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ddgi irradiance
        };
        for (int b = 0; b < 8; ++b) bindings[b].is3D = true;
        bindings[9].readonly = true;
        cascade_update_set_layout_ = device_->CreateDescriptorSetLayout({10, bindings});
    } else {
        // Metal overlapping namespaces: texture(0..3) out, texture(4..7) prev,
        // buffer(2) DDGI irradiance, buffer(3) params (setBytes override).
        // Duplicate binding numbers across the texture/buffer namespaces match
        // the finalize layout pattern.
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // cascade out L0
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // cascade out L1x
            {2, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // cascade out L1y
            {3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},  // cascade out L1z
            {4, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // cascade prev L0
            {5, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // cascade prev L1x
            {6, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // cascade prev L1y
            {7, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},  // cascade prev L1z
            {2, DescriptorType::StorageBuffer, 1, ShaderStage::Compute, nullptr},  // ddgi irradiance
            {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},  // params (setBytes)
        };
        for (int b = 0; b < 8; ++b) bindings[b].is3D = true;
        cascade_update_set_layout_ = device_->CreateDescriptorSetLayout({10, bindings});
    }
    if (cascade_update_set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[ScreenProbeGI] RadianceCascade descriptor layout creation failed" << std::endl;
        return;
    }

    PipelineLayoutDesc plDesc;
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &cascade_update_set_layout_;
    cascade_update_layout_ = device_->CreatePipelineLayout(plDesc);

    ComputePipelineDesc pipeDesc{};
    pipeDesc.computeShader = shader;
    pipeDesc.layout = cascade_update_layout_;
    pipeDesc.threadGroupSize = {4, 4, 4};  // matches the .comp local_size; 8³ groups
    cascade_update_pipeline_ = device_->CreateComputePipeline(pipeDesc);

    // Triple-buffered descriptor sets + params constant buffers.
    for (int i = 0; i < 3; ++i) {
        DescriptorSetDesc dsDesc{cascade_update_set_layout_};
        cascade_update_set_[i] = device_->CreateDescriptorSet(dsDesc);

        BufferDesc cbDesc{};
        cbDesc.size = sizeof(RadianceCascadeParams);  // 80B
        cbDesc.type = BufferType::Constant;
        cbDesc.usage = GPUMemoryUsage::Dynamic;
        cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        cascade_params_cb_[i] = device_->CreateBuffer(cbDesc);
    }

    if (cascade_update_pipeline_ == handles::INVALID_PIPELINE) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[ScreenProbeGI] [SPGI] PIPELINE INVALID: cascade_update" << std::endl;
    } else {
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[ScreenProbeGI] Radiance cascade ready (32³ RGBA16F ×2, "
            << (CascadeEnabled() ? "enabled" : "disabled via PRIMAL_GI_CASCADE=0")
            << ")" << std::endl;
    }
}

void ScreenProbeGIPass::DispatchCascadeUpdate(RHICommandBuffer* cmd, u32 frameIdx,
                                              const ResourceHandle* writeTexs,
                                              const ResourceHandle* prevTexs) {
    if (cmd == nullptr ||
        cascade_update_pipeline_ == handles::INVALID_PIPELINE ||
        cascade_update_set_[frameIdx] == handles::INVALID_DESCRIPTOR_SET ||
        frameIdx >= 3 || cascade_update_frame_ >= 0xFFFFFFFFu) {
        return;
    }
    bool texValid = writeTexs != nullptr && prevTexs != nullptr;
    if (texValid) {
        for (int c = 0; c < 4; ++c) {
            if (writeTexs[c] == handles::INVALID_RESOURCE ||
                prevTexs[c] == handles::INVALID_RESOURCE) { texValid = false; break; }
        }
    }
    if (!texValid) return;
    // Without DDGI probes there is nothing to inject — leave the cascade as-is.
    if (!ddgi_fallback_available_ || ddgi_irradiance_buffer_ == handles::INVALID_RESOURCE) {
        return;
    }
    const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;

    // Extent derivation — the finalize consumers repeat this exact formula
    // from ddgi_fb constants, so CPU and shader never drift.
    const float maxCount = std::max({ddgi_fb_counts_.x, ddgi_fb_counts_.y, ddgi_fb_counts_.z});
    const float extent = std::max(maxCount * ddgi_fb_spacing_ * 2.0f, 1.0f);
    cascade_extent_ = math::v3{extent, extent, extent};   // cubic extent as v3 (member type)
    cascade_origin_ = ddgi_fb_origin_ - math::v3{extent * 0.25f, extent * 0.25f, extent * 0.25f};
    cascade_spacing_ = extent / float(kRadianceCascadeRes);

    RadianceCascadeParams params{};
    params.origin_spacing = {cascade_origin_.x, cascade_origin_.y, cascade_origin_.z,
                             cascade_spacing_};
    params.counts_enabled = {float(kRadianceCascadeRes), float(kRadianceCascadeRes),
                             float(kRadianceCascadeRes), 1.0f};
    params.ddgi_origin_spacing = {ddgi_fb_origin_.x, ddgi_fb_origin_.y, ddgi_fb_origin_.z,
                                  ddgi_fb_spacing_};
    params.ddgi_counts = {ddgi_fb_counts_.x, ddgi_fb_counts_.y, ddgi_fb_counts_.z, 0.0f};
    params.temporal_weight = 0.9f;
    params.frame_index = float(cascade_update_frame_);

    {
        auto* cb = static_cast<RadianceCascadeParams*>(device_->MapBuffer(cascade_params_cb_[frameIdx]));
        if (cb) { *cb = params; device_->UnmapBuffer(cascade_params_cb_[frameIdx]); }
    }

    // Double buffer: the caller resolved writeTexs/prevTexs from
    // cascade_update_frame_ % 2 before import (write = frame_%2, EMA read = 1-write).
    // [0] = L0, [1..3] = L1x/L1y/L1z.

    if (isVk) {
        DescriptorData d[] = {
            {0, DescriptorType::StorageImage,  writeTexs[0]},
            {1, DescriptorType::StorageImage,  writeTexs[1]},
            {2, DescriptorType::StorageImage,  writeTexs[2]},
            {3, DescriptorType::StorageImage,  writeTexs[3]},
            {4, DescriptorType::SampledImage,  prevTexs[0]},
            {5, DescriptorType::SampledImage,  prevTexs[1]},
            {6, DescriptorType::SampledImage,  prevTexs[2]},
            {7, DescriptorType::SampledImage,  prevTexs[3]},
            {8, DescriptorType::UniformBuffer, cascade_params_cb_[frameIdx]},
            {9, DescriptorType::StorageBuffer, ddgi_irradiance_buffer_},
        };
        UpdateDescriptorSet(device_, cascade_update_set_[frameIdx], d, 10);
    } else {
        DescriptorData d[] = {
            {0, DescriptorType::StorageImage,  writeTexs[0]},
            {1, DescriptorType::StorageImage,  writeTexs[1]},
            {2, DescriptorType::StorageImage,  writeTexs[2]},
            {3, DescriptorType::StorageImage,  writeTexs[3]},
            {4, DescriptorType::SampledImage,  prevTexs[0]},
            {5, DescriptorType::SampledImage,  prevTexs[1]},
            {6, DescriptorType::SampledImage,  prevTexs[2]},
            {7, DescriptorType::SampledImage,  prevTexs[3]},
            {2, DescriptorType::StorageBuffer, ddgi_irradiance_buffer_},
            {3, DescriptorType::UniformBuffer, cascade_params_cb_[frameIdx]},
        };
        UpdateDescriptorSet(device_, cascade_update_set_[frameIdx], d, 10);
    }

    cmd->BindComputePipeline(cascade_update_pipeline_);
    const DescriptorSetHandle sets[] = { cascade_update_set_[frameIdx] };
    cmd->BindDescriptorSets(PipelineBindPoint::Compute, cascade_update_layout_, 0, 1, sets, 0, nullptr);
    if (!isVk) cmd->SetComputeBytes(3, &params, sizeof(RadianceCascadeParams));
    cmd->Dispatch(kRadianceCascadeRes / 4, kRadianceCascadeRes / 4, kRadianceCascadeRes / 4);
    SpgiStageMark(frameIdx, "cascade");

    ++cascade_update_frame_;
}

// ============================================================================
// AddPass -- main entry point called per frame
// ============================================================================

ScreenProbeGIOutput ScreenProbeGIPass::AddPass(
    RenderGraph& graph,
    RGResourceHandle gbuffer_depth,
    RGResourceHandle gbuffer_normal,
    RGResourceHandle prev_frame_color,
    const ScreenProbeCameraData& camera_data,
    u32 current_frame_index,
    RGResourceHandle hzb_texture)
{
    ScreenProbeGIOutput output{};

    u32 frameIdx = current_frame_index % 3;
    u32 histIdx = (current_frame_index + 2) % 3;
    const bool splitIsVk = device_->GetPlatform() == RHIPlatform::Vulkan;

    // Import persistent output textures into render graph
    auto giOutputHandle = graph.ImportResource("ScreenProbeGI_Output", output_texture_);
    auto giFilteredHandle = graph.ImportResource("ScreenProbeGI_Filtered", output_texture_filtered_);
    // T5.3: denoise re-enabled. The historical "solid color in mode 6" came from
    // the descriptor-update bug fixed in the denoise dispatch (Vulkan bindings
    // clashed at 0 and the params UBO was never bound). Route diagnostics that
    // skip denoise/post to the Gather texture: the filtered texture has no
    // producer in those modes, so exposing it would sample an unwritten image
    // and make the A/B look like SPGI was disabled.
    const bool denoiseOutput = denoise_pipeline_ != handles::INVALID_PIPELINE &&
                               !SpgiStageSkipped("post") &&
                               !SpgiStageSkipped("denoise");
    output.gi_output = denoiseOutput ? giFilteredHandle : giOutputHandle;

    // Import surface cache resources into render graph (needed for RGResourceHandle conversion)
    RGResourceHandle scLightingHandle;
    RGResourceHandle scCardDataHandle;
    RGResourceHandle scCardLookupHandle;
    RGResourceHandle scDepthHandle;
    if (surface_cache_available_) {
        // T1.1 (C2): reuse the pipeline-level "SCLightingAtlas" import when
        // provided so the atlas has exactly one RG identity per frame.
        scLightingHandle = external_lighting_rg_.IsValid()
            ? external_lighting_rg_
            : graph.ImportResource("SurfaceCache_Lighting_ForProbes", surface_cache_lighting_atlas_);
        if (surface_cache_depth_atlas_ != handles::INVALID_RESOURCE)
            scDepthHandle = graph.ImportResource("SurfaceCache_Depth_ForProbes", surface_cache_depth_atlas_);
        scCardDataHandle = graph.ImportResource("SurfaceCache_CardData_ForProbes", surface_cache_card_data_buffer_);
        scCardLookupHandle = graph.ImportResource("SurfaceCache_CardLookup_ForProbes", surface_cache_card_lookup_buffer_);
    }

    // T3.5: import the cascade double buffer for this frame. Write side gets
    // UnorderedAccess (storage image), prev side ShaderResource (sampled by
    // the cascade update kernel AND the finalize far-cache — one frame stale,
    // which the temporal EMA already implies). RG-tracked transitions preserve
    // the cross-frame contents (Vulkan oldLayout comes from the texture).
    const bool cascadeActive = CascadeEnabled() &&
        cascade_update_pipeline_ != handles::INVALID_PIPELINE &&
        radiance_cascade_[0] != handles::INVALID_RESOURCE &&
        radiance_cascade_[1] != handles::INVALID_RESOURCE &&
        radiance_cascade_l1_[0][0] != handles::INVALID_RESOURCE &&
        radiance_cascade_l1_[1][0] != handles::INVALID_RESOURCE;
    const u32 cascadeWriteIdx = cascade_update_frame_ % 2;
    // SH-L1: one write/prev import pair per coefficient — [0] = L0,
    // [1..3] = L1x/L1y/L1z. Unique import names keep the RG identities distinct.
    static constexpr const char* kCascadeCoeffNames[4] = {"L0", "L1x", "L1y", "L1z"};
    RGResourceHandle cascadeWriteRG[4];
    RGResourceHandle cascadePrevRG[4];
    if (cascadeActive) {
        for (int c = 0; c < 4; ++c) {
            cascadeWriteRG[c] = graph.ImportResource(
                std::string("RadianceCascade_Write_") + kCascadeCoeffNames[c],
                (c == 0) ? radiance_cascade_[cascadeWriteIdx]
                         : radiance_cascade_l1_[cascadeWriteIdx][c - 1]);
            cascadePrevRG[c] = graph.ImportResource(
                std::string("RadianceCascade_Prev_") + kCascadeCoeffNames[c],
                (c == 0) ? radiance_cascade_[1 - cascadeWriteIdx]
                         : radiance_cascade_l1_[1 - cascadeWriteIdx][c - 1]);
        }
    }

    graph.AddPass<ScreenProbePassData>("ScreenProbeGI",
        RGPassType::Compute, RGPassCategory::Lighting,

        // ====================================================================
        // Setup lambda: declare resource dependencies
        // ====================================================================
        [this, gbuffer_depth, gbuffer_normal, prev_frame_color, hzb_texture, giOutputHandle, giFilteredHandle,
         scLightingHandle, scCardDataHandle, scCardLookupHandle, scDepthHandle, cascadeWriteRG, cascadePrevRG](
            ScreenProbePassData& data, RenderGraphBuilder& builder) {
            // Read GBuffer inputs
            builder.Read(gbuffer_depth, ResourceState::ShaderResource);
            builder.Read(gbuffer_normal, ResourceState::ShaderResource);

            // Read previous frame color for radiance sampling
            if (prev_frame_color.IsValid()) {
                builder.Read(prev_frame_color, ResourceState::ShaderResource);
            }

            // T5.1: HZB near-field input (ordering vs. HZBBuild)
            if (hzb_texture.IsValid()) {
                builder.Read(hzb_texture, ResourceState::ShaderResource);
            }

            // Read surface cache resources for near-hit sampling
            if (surface_cache_available_) {
                builder.Read(scLightingHandle, ResourceState::ShaderResource);
                if (scDepthHandle.IsValid()) builder.Read(scDepthHandle, ResourceState::ShaderResource);
                builder.Read(scCardDataHandle, ResourceState::ShaderResource);
                builder.Read(scCardLookupHandle, ResourceState::ShaderResource);
            }

            // Write to raw output texture (Gather output)
            builder.Write(giOutputHandle, ResourceState::UnorderedAccess);
            // Write to filtered output texture (Denoise output)
            builder.Write(giFilteredHandle, ResourceState::UnorderedAccess);

            // T3.5: cascade storage-image writes + sampled reads of the EMA
            // histories (L0 + L1x/L1y/L1z since the SH-L1 upgrade).
            for (int c = 0; c < 4; ++c) {
                if (cascadeWriteRG[c].IsValid()) builder.Write(cascadeWriteRG[c], ResourceState::UnorderedAccess);
                if (cascadePrevRG[c].IsValid())  builder.Read(cascadePrevRG[c], ResourceState::ShaderResource);
            }

            data.gbuffer_depth = gbuffer_depth;
            data.gbuffer_normal = gbuffer_normal;
            data.prev_frame_color = prev_frame_color;
            data.gi_output = giOutputHandle;
            for (int c = 0; c < 4; ++c) {
                data.cascade_write[c] = cascadeWriteRG[c];
                data.cascade_prev[c] = cascadePrevRG[c];
            }
        },

        // ====================================================================
        // Execute lambda: dispatch 3 compute sub-passes
        // ====================================================================
        [this, camera_data, frameIdx, histIdx, giFilteredHandle, splitIsVk,
         cascadeActive, cascadeWriteRG, cascadePrevRG](
            const ScreenProbePassData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            // Keep the HZB bisect single-stage: when skipped, both the dispatch
            // and the shader's early HZB-hit gate are disabled. Otherwise SDF
            // could observe a stale w==1 entry from the shared hit buffer and
            // the result would not be a valid HZB-vs-SDF comparison.
            const bool skipHZB = SpgiStageSkipped("hzb");

            // Resolve physical handles from render graph
            // P0-B（01197 定位）: storage 写入前确保 GENERAL。帧内
            // "restore→SRO 后又被本 pass 后续子阶段 storage 写"的序列不会自动
            // 补 SRO→GENERAL（RG 只在 pass 入口发一次），tracked/real 从此
            // 失步。UA→UA barrier 由后端按 tracked 解析：已是 GENERAL 时
            // no-op，停在 SRO 时发转换。
            auto EnsureStorageWritable = [&](ResourceHandle tex) {
                if (tex == handles::INVALID_RESOURCE) return;
                ResourceBarrier b{};
                b.resource = tex;
                b.beforeState = ResourceState::UnorderedAccess;
                b.afterState  = ResourceState::UnorderedAccess;
                b.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&b, 1);
            };
            auto ResolveTexture = [&](RGResourceHandle handle) -> ResourceHandle {
                auto* res = context.graph->GetResource(handle);
                if (res) return res->GetPhysicalHandle();
                return handles::INVALID_RESOURCE;
            };

            ResourceHandle depthTex = ResolveTexture(data.gbuffer_depth);
            ResourceHandle normalTex = ResolveTexture(data.gbuffer_normal);
            ResourceHandle prevColorTex = ResolveTexture(data.prev_frame_color);
            ResourceHandle outputTex = ResolveTexture(data.gi_output);
            ResourceHandle filteredTex = ResolveTexture(giFilteredHandle);

            if (depthTex == handles::INVALID_RESOURCE ||
                normalTex == handles::INVALID_RESOURCE ||
                outputTex == handles::INVALID_RESOURCE) {
                return;
            }

            // Resolve SDF cascade textures — offline high-res volume (single
            // cascade, aliased to keep descriptor slots valid) or runtime
            // GlobalSDF. The offline field traces at ~0.17m voxels vs the
            // runtime 1m/2m/4m unsigned cascades.
            bool sdfAvailable = false;
            ResourceHandle sdfTextures[3] = {
                handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, handles::INVALID_RESOURCE
            };
            if (has_offline_sdf_) {
                sdfTextures[0] = offline_sdf_texture_;
                sdfTextures[1] = offline_sdf_texture_;   // alias (cascadeCount=1)
                sdfTextures[2] = offline_sdf_texture_;
                sdfAvailable = (offline_sdf_texture_ != handles::INVALID_RESOURCE);
            } else {
                auto& sdf = nanite::GlobalSDF::Get();
                sdfAvailable = sdf.IsInitialized();
                if (sdfAvailable) {
                    for (u32 c = 0; c < std::min(3u, sdf.GetConfig().cascade_count); ++c) {
                        sdfTextures[c] = sdf.GetCascade(c).sdf_texture;
                    }
                }
            }

            // DEBUG: log first 5 frames
            static u32 dbgSpFrame = 0;
            if (dbgSpFrame < 20) {
                PRIMAL_LOG_STREAM(Info, "Render") << "[ScreenProbeGI] Frame " << dbgSpFrame
                          << " sdf=" << (sdfAvailable ? "YES" : "NO")
                          << " prevColor=" << prevColorTex
                          << " scAvail=" << (surface_cache_available_ ? "YES" : "NO")
                          << " depth=" << depthTex
                          << " normal=" << normalTex
                          << " outputTex(RG)=" << outputTex
                          << " output_texture_(member)=" << output_texture_
                          << " filteredTex=" << filteredTex
                          << " output_texture_filtered_(member)=" << output_texture_filtered_
                          << " denoise_pipe=" << denoise_pipeline_
                          << " place_pipe=" << place_pipeline_
                          << " trace_pipe=" << trace_pipeline_
                          << " gather_pipe=" << gather_pipeline_
                          << " grid=" << grid_width_ << "x" << grid_height_
                          << std::endl;
                dbgSpFrame++;
            }

            // ---- Prepare constant buffer data ----
            // Use SetComputeBytes (setBytes) instead of pool-allocated buffer
            // to bypass triple-buffer synchronization issues with memory pool
            ScreenProbeGlobalData gd{};
            {
                math::m4x4 vp = camera_data.proj_matrix * camera_data.view_matrix;
                gd.view_projection = vp;
                gd.inv_view_projection = rhi::math::Inverse(vp);
                // F5 (M7): camera_position.w carries the jitter seed (frame
                // index; negative disables). The Average kernel receives the
                // same value through its AvgConstants pad slot.
                const float jitterSeed = params_.enable_jitter
                    ? static_cast<float>(camera_data.frame_index) : -1.0f;
                gd.camera_position = {camera_data.camera_position.x,
                                      camera_data.camera_position.y,
                                      camera_data.camera_position.z, jitterSeed};
                gd.grid_params = {(float)grid_width_, (float)grid_height_,
                                  (float)params_.downsample_factor, (float)params_.rays_per_probe};
                gd.trace_params = {params_.max_ray_distance, params_.gather_radius,
                                   (float)render_width_, (float)render_height_};

                if (sdfAvailable && has_offline_sdf_) {
                    // Offline single-volume fill (mirrors LumenDDGIPass): one
                    // cascade, voxel = maxExtent/res; sampleBestSDF's
                    // cascadeCount=1 path handles the rest.
                    const float maxExt = std::max({offline_sdf_extent_.x,
                                                   offline_sdf_extent_.y,
                                                   offline_sdf_extent_.z});
                    const float vs = maxExt / static_cast<float>(offline_sdf_resolution_);
                    gd.sdf_origins[0] = {offline_sdf_origin_.x, offline_sdf_origin_.y,
                                         offline_sdf_origin_.z, 0.0f};
                    gd.sdf_voxel_sizes[0] = {vs, vs, vs, 0.0f};
                    gd.sdf_extents[0] = {offline_sdf_extent_.x, offline_sdf_extent_.y,
                                         offline_sdf_extent_.z, 0.0f};
                    gd.sdf_resolutions = {static_cast<float>(offline_sdf_resolution_),
                                          0.0f, 0.0f, 1.0f};
                } else if (sdfAvailable) {
                    field::FieldDescriptor sdf_cascades[4];
                    u32 cascadeCount = field::FieldRegistry::Get().FindCascaded(
                        field::FieldSemantic::GlobalSDF, sdf_cascades, 4);
                    if (cascadeCount > 0) {
                        field::FieldView::WriteSDFToScreenProbe(sdf_cascades, cascadeCount, gd);
                    }
                }

                gd.surface_cache_params = {
                    surface_cache_available_ ? static_cast<float>(surface_cache_atlas_size_) : 0.0f,
                    static_cast<float>(surface_cache_card_count_),
                    surface_cache_available_ ? 1.0f : 0.0f,
                    // F5: jitter world-space amplitude clamp (read by Place;
                    // 0 disables together with a negative seed).
                    params_.enable_jitter ? params_.jitter_max_world : 0.0f
                };

                // T2.2: DDGI probe fallback params for miss rays
                gd.ddgi_fb[0] = {ddgi_fb_origin_.x, ddgi_fb_origin_.y, ddgi_fb_origin_.z,
                                 ddgi_fb_spacing_};
                gd.ddgi_fb[1] = {ddgi_fb_counts_.x, ddgi_fb_counts_.y, ddgi_fb_counts_.z,
                                 ddgi_fallback_available_ ? 1.0f : 0.0f};

                // T5.1: HZB near-field march params (SSGI/SSR near-far convention).
                gd.hzb_params = {0.1f, 1000.0f,
                                 (!skipHZB && hzb_trace_pipeline_ != handles::INVALID_PIPELINE &&
                                  hzb_texture_ != handles::INVALID_RESOURCE && hzb_mip_levels_ > 0)
                                     ? (float)hzb_mip_levels_ : 0.0f,
                                 params_.hzb_max_distance};

                // F1: far-cache early-out distance (0 disables); F2: y = importance;
                // F4: z = adaptive interpolation-error threshold (0 → legacy);
                // T3.5: w = radiance cascade far-cache fast path (0 → legacy
                // 16-read tetrahedral query).
                gd.cache_params = {params_.far_cache_start_distance,
                                   params_.enable_importance ? 1.0f : 0.0f,
                                   params_.adaptive_pos_threshold,
                                   cascadeActive ? 1.0f : 0.0f};
                // P1（reaudit §4.1/4.2）: trace 诊断模式（0=off 默认零开销）
                // G2 A/B（2026-09-16）: y = SC 候选朝向门阈值（n_true·hint
                // 低于阈值拒绝）。PRIMAL_SPGI_FACE_GATE 环境可调（0.5 默认，
                // 0.0=仅拒绝明显异面，-1=门关闭）；z 非 0 表示 hint 用三线性
                // 梯度（PRIMAL_SPGI_TRILINEAR_HINT）。
                static const float kFaceGate = [] {
                    if (const char* e = std::getenv("PRIMAL_SPGI_FACE_GATE")) {
                        char* end = nullptr;
                        float v = std::strtof(e, &end);
                        if (end && *end == '\0') return v;
                    }
                    // 2026-09-16 A/B 实测定档：0.5→0.25。PCSS 卡片修复后
                    // 门限 0.25 与 0.0 的 arch/darkarch 差异在噪声内，而
                    // darkarch（合法反弹）+0.8；门全关 arch 回跳 17→21。
                    return 0.25f;
                }();
                static const float kTrilinearHint = [] {
                    const char* e = std::getenv("PRIMAL_SPGI_TRILINEAR_HINT");
                    return (e && e[0] != '0') ? 1.0f : 0.0f;
                }();
                gd.diag_params = {static_cast<float>(trace_diag_mode_), kFaceGate, kTrilinearHint, 0.0f};
                gd.hint_params = {kTrilinearHint, 0.0f, 0.0f, 0.0f};
            }

            // Split kernels take the global data through the pool UBO
            // (Vulkan flat bindings) — upload alongside the setBytes path.
            {
                auto* gdCb = static_cast<ScreenProbeGlobalData*>(device_->MapBuffer(global_cb_[frameIdx]));
                if (gdCb) { *gdCb = gd; device_->UnmapBuffer(global_cb_[frameIdx]); }
            }

            // ==================================================================
            // Sub-pass 1: ScreenProbePlace
            // ==================================================================
            {
                // Vulkan: flat bindings (images 0-1, SSBO 2-3, UBO 4) matching
                // ScreenProbePlace.comp; Metal: overlapping texture/buffer(0,1)
                // namespaces with setBytes(3).
                DescriptorData params[5];
                if (splitIsVk) {
                    DescriptorData p[] = {
                        {0, DescriptorType::SampledImage,  depthTex},
                        {1, DescriptorType::SampledImage,  normalTex},
                        {2, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                        {3, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                        {4, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    };
                    memcpy(params, p, sizeof(p));
                } else {
                    DescriptorData p[] = {
                        {0, DescriptorType::SampledImage,  depthTex},
                        {1, DescriptorType::SampledImage,  normalTex},
                        {0, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                        {1, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                        {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                    };
                    memcpy(params, p, sizeof(p));
                }
                UpdateDescriptorSet(device_, place_ds_[frameIdx], params, 5);

                cmd->BindComputePipeline(place_pipeline_);
                const DescriptorSetHandle sets[] = { place_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, place_layout_, 0, 1, sets, 0, nullptr);
                if (!splitIsVk) cmd->SetComputeBytes(3, &gd, sizeof(ScreenProbeGlobalData));

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch((totalProbes + 63) / 64, 1, 1);
                SpgiStageMark(frameIdx, "place");
            }

            // Barrier: probe buffers written
            {
                ResourceBarrier barriers[2]{};
                barriers[0].resource = probe_positions_buffer_[frameIdx];
                barriers[0].beforeState = ResourceState::UnorderedAccess;
                barriers[0].afterState = ResourceState::ShaderResource;
                barriers[0].subresource = 0xFFFFFFFF;
                barriers[1].resource = probe_normals_buffer_[frameIdx];
                barriers[1].beforeState = ResourceState::UnorderedAccess;
                barriers[1].afterState = ResourceState::ShaderResource;
                barriers[1].subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(barriers, 2);
            }

            // ==================================================================
            // Sub-pass 2: hybrid trace (T5.1)
            //   2a HZB near-field (texture2D mip chain, sharp screen detail)
            //   2b SDF far-field  (texture3D cascades, skips 2a-resolved rays)
            //   2c Finalize       (texture2D + buffers radiance chain)
            // Monolithic trace is the fallback when split pipelines are absent.
            // ==================================================================
            if (sdfAvailable && prevColorTex != handles::INVALID_RESOURCE) {
                ResourceHandle scDepthAtlas = surface_cache_depth_atlas_ != handles::INVALID_RESOURCE ? surface_cache_depth_atlas_ : dummy_texture_2d_;
                ResourceHandle scLightingAtlas = surface_cache_available_ ? surface_cache_lighting_atlas_ : dummy_texture_2d_;
                ResourceHandle scCardData = surface_cache_available_ ? surface_cache_card_data_buffer_ : dummy_buffer_;
                ResourceHandle scCardLookup = surface_cache_available_ ? surface_cache_card_lookup_buffer_ : dummy_buffer_;
                ResourceHandle scGridOffsets = (surface_cache_available_ &&
                    surface_cache_grid_offsets_buffer_ != handles::INVALID_RESOURCE)
                    ? surface_cache_grid_offsets_buffer_ : dummy_buffer_;
                ResourceHandle scGridCards = (surface_cache_available_ &&
                    surface_cache_grid_cards_buffer_ != handles::INVALID_RESOURCE)
                    ? surface_cache_grid_cards_buffer_ : dummy_buffer_;
                ResourceHandle ddgiIrr = (ddgi_fallback_available_ &&
                    ddgi_irradiance_buffer_ != handles::INVALID_RESOURCE)
                    ? ddgi_irradiance_buffer_ : dummy_buffer_;

                u32 totalRays = grid_width_ * grid_height_ * params_.rays_per_probe;
                u32 dispatchX = (totalRays + 63) / 64;
                // Bring-up: per-frame trace dispatch params + bound resources.
                // The device hang reproduces at the pass's frame 2 (render
                // frame 1) — frame-over-frame deltas are the diagnostic.
                {
                    static u32 resDumps = 0;
                    if (resDumps < 6) {
                        ++resDumps;
                        PRIMAL_LOG_STREAM(Info, "Render")
                            << "[SPGI-RES] frameIdx=" << frameIdx << " histIdx=" << histIdx
                            << " dispatchX=" << dispatchX << " totalRays=" << totalRays
                            << " hitBuf=" << hit_distance_buffer_
                            << " radBuf[" << frameIdx << "]=" << probe_radiance_buffer_[frameIdx]
                            << " sdfTex=" << sdfTextures[0] << "," << sdfTextures[1] << "," << sdfTextures[2]
                            << " hzbTex=" << hzb_texture_ << " hzbMips=" << hzb_mip_levels_
                            << " prevColor=" << prevColorTex
                            << " gridOff=" << scGridOffsets << " gridCards=" << scGridCards
                            << " ddgiIrr=" << ddgiIrr << std::endl;
                    }
                }

                const bool splitReady = (sdf_trace_pipeline_ != handles::INVALID_PIPELINE &&
                                         finalize_pipeline_ != handles::INVALID_PIPELINE &&
                                         hit_distance_buffer_ != handles::INVALID_RESOURCE);
                const bool hzbReady = (!skipHZB && hzb_trace_pipeline_ != handles::INVALID_PIPELINE &&
                                      hzb_texture_ != handles::INVALID_RESOURCE && hzb_mip_levels_ > 0);

                if (splitReady && !SpgiStageSkipped("trace")) {
                    // --- T3.5: cascade update (before any far-cache consumer) ---
                    // SH-L1: [0] = L0, [1..3] = L1x/L1y/L1z physical handles;
                    // cascadePrevTex feeds the finalize far-cache bindings.
                    ResourceHandle cascadeWriteTexs[4];
                    ResourceHandle cascadePrevTex[4];
                    bool cascadeReady = cascadeActive;
                    for (int c = 0; c < 4; ++c) {
                        cascadeWriteTexs[c] = handles::INVALID_RESOURCE;
                        cascadePrevTex[c] = handles::INVALID_RESOURCE;
                    }
                    if (cascadeReady && cascadeWriteRG[0].IsValid() && cascadePrevRG[0].IsValid()) {
                        for (int c = 0; c < 4; ++c) {
                            auto* resW = context.graph->GetResource(cascadeWriteRG[c]);
                            auto* resP = context.graph->GetResource(cascadePrevRG[c]);
                            cascadeWriteTexs[c] = resW ? resW->GetPhysicalHandle() : handles::INVALID_RESOURCE;
                            cascadePrevTex[c] = resP ? resP->GetPhysicalHandle() : handles::INVALID_RESOURCE;
                            if (cascadeWriteTexs[c] == handles::INVALID_RESOURCE ||
                                cascadePrevTex[c] == handles::INVALID_RESOURCE) { cascadeReady = false; break; }
                        }
                    }
                    if (cascadeReady) {
                        DispatchCascadeUpdate(cmd, frameIdx, cascadeWriteTexs, cascadePrevTex);
                        // Storage-image write → sampled reads of the same
                        // textures (cascade EMA read next frame + finalize
                        // this frame).
                        cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                           AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
                    }
                    // --- 2a: HZB near-field (optional first stage) ---
                    if (hzbReady) {
                        // Infer the descriptor count: importance sampling adds a
                        // sixth binding after the original five HZB resources.
                        const DescriptorData hzbParams[] = {
                            {0, DescriptorType::SampledImage, hzb_texture_},
                            {splitIsVk ? 1u : 0u, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                            {splitIsVk ? 2u : 1u, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                            {splitIsVk ? 3u : 2u, DescriptorType::StorageBuffer, hit_distance_buffer_},
                            {splitIsVk ? 4u : 3u, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                            {splitIsVk ? 5u : 4u, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},
                        };
                        UpdateDescriptorSet(device_, hzb_trace_ds_[frameIdx], hzbParams,
                                            static_cast<uint32_t>(std::size(hzbParams)));
                        cmd->BindComputePipeline(hzb_trace_pipeline_);
                        const DescriptorSetHandle sets[] = { hzb_trace_ds_[frameIdx] };
                        cmd->BindDescriptorSets(PipelineBindPoint::Compute, hzb_trace_layout_, 0, 1, sets, 0, nullptr);
                        if (!splitIsVk) cmd->SetComputeBytes(3, &gd, sizeof(ScreenProbeGlobalData));
                        cmd->Dispatch(dispatchX, 1, 1);
                        SpgiStageMark(frameIdx, "hzb");
                        // L2 visibility before the SDF kernel reads the hit buffer.
                        cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                           AccessFlag::ShaderWrite, AccessFlag::ShaderRead);
                    }
                    // Without HZB, SDF overwrites every ray on the GPU. Never
                    // memset this shared intermediate while earlier frames may
                    // still be using it.

                    // --- 2b: SDF far-field (pure texture3D + probe buffers) ---
                    // T4.3 二批: upload the detail slot table + bind the extra
                    // entries (unused slots fall back to sdfTextures[0] — the
                    // shader never samples them when header.count is 0).
                    if (detail_table_cb_[frameIdx] != handles::INVALID_RESOURCE) {
                        struct DetailTableGPU {
                            math::v4 header;
                            math::m4x4 inv_world[4];
                            math::v4 normal_matrix[4][3];
                            math::v4 bounds_min[4];
                            math::v4 bounds_max[4];
                            math::v4 params[4];
                        };
                        static_assert(sizeof(DetailTableGPU) == 656, "detail table layout drift");
                        if (auto* tm = static_cast<DetailTableGPU*>(device_->MapBuffer(detail_table_cb_[frameIdx]))) {
                            tm->header = math::v4{(f32)detail_sdf_slot_count_, 0.f, 0.f, 0.f};
                            for (u32 s = 0; s < 4; ++s) {
                                if (s < detail_sdf_slot_count_) {
                                    tm->inv_world[s] = detail_sdf_slots_[s].inv_world;
                                    for (u32 r = 0; r < 3; ++r)
                                        tm->normal_matrix[s][r] = detail_sdf_slots_[s].normal_matrix[r];
                                    tm->bounds_min[s] = detail_sdf_slots_[s].bounds_min;
                                    tm->bounds_max[s] = detail_sdf_slots_[s].bounds_max;
                                    tm->params[s] = detail_sdf_slots_[s].params;
                                } else {
                                    tm->inv_world[s] = math::m4x4{};
                                    tm->normal_matrix[s][0] = tm->normal_matrix[s][1] = math::v4{};
                                    tm->normal_matrix[s][2] = math::v4{};
                                    tm->bounds_min[s] = tm->bounds_max[s] = math::v4{};
                                    tm->params[s] = math::v4{};
                                }
                            }
                            device_->UnmapBuffer(detail_table_cb_[frameIdx]);
                        }
                    }
                    ResourceHandle detailTex[kDetailSDFSlots];
                    for (u32 s = 0; s < kDetailSDFSlots; ++s) {
                        detailTex[s] = (s < detail_sdf_slot_count_ &&
                                        detail_sdf_textures_[s] != handles::INVALID_RESOURCE)
                            ? detail_sdf_textures_[s] : sdfTextures[0];
                    }
                    if (splitIsVk) {
                        DescriptorData sdfParams[] = {
                            {0, DescriptorType::SampledImage,  sdfTextures[0]},
                            {1, DescriptorType::SampledImage,  sdfTextures[1]},
                            {2, DescriptorType::SampledImage,  sdfTextures[2]},
                            {3, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                            {4, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                            {5, DescriptorType::StorageBuffer, hit_distance_buffer_},
                            {6, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                            {7, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},  // F2
                            {8,  DescriptorType::UniformBuffer, detail_table_cb_[frameIdx]},   // T4.3 二批
                            {9,  DescriptorType::SampledImage,  detailTex[0]},
                            {10, DescriptorType::SampledImage,  detailTex[1]},
                            {11, DescriptorType::SampledImage,  detailTex[2]},
                            {12, DescriptorType::SampledImage,  detailTex[3]},
                        };
                        UpdateDescriptorSet(device_, sdf_trace_ds_[frameIdx], sdfParams, 13);
                    } else {
                        DescriptorData sdfParams[] = {
                            {0, DescriptorType::SampledImage,  sdfTextures[0]},
                            {1, DescriptorType::SampledImage,  sdfTextures[1]},
                            {2, DescriptorType::SampledImage,  sdfTextures[2]},
                            {0, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                            {1, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                            {2, DescriptorType::StorageBuffer, hit_distance_buffer_},
                            {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                            {4, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},  // F2
                            {5, DescriptorType::UniformBuffer, detail_table_cb_[frameIdx]},    // T4.3 二批
                            {3, DescriptorType::SampledImage,  detailTex[0]},
                            {4, DescriptorType::SampledImage,  detailTex[1]},
                            {5, DescriptorType::SampledImage,  detailTex[2]},
                            {6, DescriptorType::SampledImage,  detailTex[3]},
                        };
                        UpdateDescriptorSet(device_, sdf_trace_ds_[frameIdx], sdfParams, 13);
                    }
                    cmd->BindComputePipeline(sdf_trace_pipeline_);
                    const DescriptorSetHandle sdfSets[] = { sdf_trace_ds_[frameIdx] };
                    cmd->BindDescriptorSets(PipelineBindPoint::Compute, sdf_trace_layout_, 0, 1, sdfSets, 0, nullptr);
                    if (!splitIsVk) cmd->SetComputeBytes(3, &gd, sizeof(ScreenProbeGlobalData));
                    cmd->Dispatch(dispatchX, 1, 1);
                    SpgiStageMark(frameIdx, "sdf");
                    cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::ComputeShader,
                                       AccessFlag::ShaderWrite, AccessFlag::ShaderRead);

                    // --- 2c: radiance finalize (texture2D + buffers) ---
                    // T3.5: binding 13 / texture(2) carries the cascade L0;
                    // SH-L1 adds bindings 14-16 / textures (3)-(5) for L1xyz.
                    // The finalize samples the PREV import identity — one frame
                    // stale, which the cascade's temporal EMA already implies,
                    // and the RG has it in ShaderResource layout for the
                    // SampledImage descriptor without a mid-pass transition.
                    ResourceHandle cascadeFinalTex[4];
                    for (int c = 0; c < 4; ++c) {
                        cascadeFinalTex[c] = (cascadeReady && cascadePrevTex[c] != handles::INVALID_RESOURCE)
                            ? cascadePrevTex[c] : dummy_texture_3d_;
                    }
                    if (splitIsVk) {
                        DescriptorData finParams[] = {
                            {0, DescriptorType::SampledImage,  prevColorTex},
                            {1, DescriptorType::SampledImage,  scLightingAtlas},
                            {2, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                            {3, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                            {4, DescriptorType::StorageBuffer, hit_distance_buffer_},
                            {5, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                            {6, DescriptorType::StorageBuffer, probe_radiance_buffer_[frameIdx]},
                            {7, DescriptorType::StorageBuffer, scCardData},
                            {8, DescriptorType::StorageBuffer, scCardLookup},
                            {9, DescriptorType::StorageBuffer, scGridOffsets},
                            {10, DescriptorType::StorageBuffer, scGridCards},
                            {11, DescriptorType::StorageBuffer, ddgiIrr},
                            {12, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},  // F2
                            {17, DescriptorType::SampledImage, scDepthAtlas},
                            {13, DescriptorType::SampledImage, cascadeFinalTex[0]},  // T3.5 L0
                            {14, DescriptorType::SampledImage, cascadeFinalTex[1]},  // SH-L1 L1x
                            {15, DescriptorType::SampledImage, cascadeFinalTex[2]},  // SH-L1 L1y
                            {16, DescriptorType::SampledImage, cascadeFinalTex[3]},  // SH-L1 L1z
                            {18, DescriptorType::StorageBuffer,
                                trace_stats_buffer_ != handles::INVALID_RESOURCE
                                    ? trace_stats_buffer_ : dummy_buffer_},          // P1: 分支统计
                        };
                        UpdateDescriptorSet(device_, finalize_ds_[frameIdx], finParams, 19);
                    } else {
                        DescriptorData finParams[] = {
                            {0, DescriptorType::SampledImage,  prevColorTex},
                            {1, DescriptorType::SampledImage,  scLightingAtlas},
                            {2, DescriptorType::SampledImage,  cascadeFinalTex[0]},  // T3.5 L0
                            {3, DescriptorType::SampledImage,  cascadeFinalTex[1]},  // SH-L1 L1x
                            {4, DescriptorType::SampledImage,  cascadeFinalTex[2]},  // SH-L1 L1y
                            {5, DescriptorType::SampledImage,  cascadeFinalTex[3]},  // SH-L1 L1z
                            {0, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                            {1, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                            {2, DescriptorType::StorageBuffer, hit_distance_buffer_},
                            {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                            {4, DescriptorType::StorageBuffer, probe_radiance_buffer_[frameIdx]},
                            {5, DescriptorType::StorageBuffer, scCardData},
                            {6, DescriptorType::StorageBuffer, scCardLookup},
                            {7, DescriptorType::StorageBuffer, scGridOffsets},
                            {8, DescriptorType::StorageBuffer, scGridCards},
                            {9, DescriptorType::StorageBuffer, ddgiIrr},
                            {10, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},  // F2
                            {6, DescriptorType::SampledImage, scDepthAtlas},
                        };
                        UpdateDescriptorSet(device_, finalize_ds_[frameIdx], finParams, 18);
                    }
                    EnsureStorageWritable(outputTex);
                    cmd->BindComputePipeline(finalize_pipeline_);
                    const DescriptorSetHandle finSets[] = { finalize_ds_[frameIdx] };
                    cmd->BindDescriptorSets(PipelineBindPoint::Compute, finalize_layout_, 0, 1, finSets, 0, nullptr);
                    if (!splitIsVk) cmd->SetComputeBytes(3, &gd, sizeof(ScreenProbeGlobalData));
                    cmd->Dispatch(dispatchX, 1, 1);
                    SpgiStageMark(frameIdx, "finalize");
                } else if (!SpgiStageSkipped("trace")) {
                    // Monolithic fallback — Vulkan flat bindings 0..13 (fixed:
                    // previously wrote Metal overlapping numbers on both platforms).
                    if (splitIsVk) {
                        DescriptorData params[] = {
                            {0, DescriptorType::SampledImage,  sdfTextures[0]},
                            {1, DescriptorType::SampledImage,  sdfTextures[1]},
                            {2, DescriptorType::SampledImage,  sdfTextures[2]},
                            {3, DescriptorType::SampledImage,  prevColorTex},
                            {4, DescriptorType::SampledImage,  scLightingAtlas},
                            {5, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                            {6, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                            {7, DescriptorType::StorageBuffer, probe_radiance_buffer_[frameIdx]},
                            {8, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                            {9, DescriptorType::StorageBuffer, scCardData},
                            {10, DescriptorType::StorageBuffer, scCardLookup},
                            {11, DescriptorType::StorageBuffer, scGridOffsets},
                            {12, DescriptorType::StorageBuffer, scGridCards},
                            {13, DescriptorType::StorageBuffer, ddgiIrr},
                            {14, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},  // F2
                        };
                        UpdateDescriptorSet(device_, trace_ds_[frameIdx], params, 15);
                    } else {
                        DescriptorData params[] = {
                            {0, DescriptorType::SampledImage,  sdfTextures[0]},
                            {1, DescriptorType::SampledImage,  sdfTextures[1]},
                            {2, DescriptorType::SampledImage,  sdfTextures[2]},
                            {3, DescriptorType::SampledImage,  prevColorTex},
                            {4, DescriptorType::SampledImage,  scLightingAtlas},
                            {0, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                            {1, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                            {2, DescriptorType::StorageBuffer, probe_radiance_buffer_[frameIdx]},
                            {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                            {4, DescriptorType::StorageBuffer, scCardData},
                            {5, DescriptorType::StorageBuffer, scCardLookup},
                            {6, DescriptorType::StorageBuffer, scGridOffsets},
                            {7, DescriptorType::StorageBuffer, scGridCards},
                            {8, DescriptorType::StorageBuffer, ddgiIrr},
                            {9, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},  // F2
                        };
                        UpdateDescriptorSet(device_, trace_ds_[frameIdx], params, 15);
                    }
                    EnsureStorageWritable(outputTex);
                    cmd->BindComputePipeline(trace_pipeline_);
                    const DescriptorSetHandle sets[] = { trace_ds_[frameIdx] };
                    cmd->BindDescriptorSets(PipelineBindPoint::Compute, trace_layout_, 0, 1, sets, 0, nullptr);
                    cmd->SetComputeBytes(3, &gd, sizeof(ScreenProbeGlobalData));
                    cmd->Dispatch(dispatchX, 1, 1);
                    SpgiStageMark(frameIdx, "monolith");
                }
            }

            // Barrier: radiance buffer written
            {
                ResourceBarrier barrier{};
                barrier.resource = probe_radiance_buffer_[frameIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 2.5: ScreenProbeAverage (SH2 accumulation)
            // ==================================================================
            // Average is the first materialized SH result. Keep it enabled
            // when `post` is skipped so PRIMAL_SPGI_SKIP=post still feeds the
            // raw current-frame SH into Gather; otherwise the diagnostic path
            // would consume the untouched (zeroed) history slot and look the
            // same as disabling SPGI entirely.
            if (!SpgiStageSkipped("average")) {
                struct AvgCB { u32 totalProbes; u32 raysPerProbe; float pad[2]; };
                // pad[0] = F5 jitter seed — MUST equal camera_position.w so the
                // SH projection regenerates the same rotated ray directions as
                // the trace kernels. pad[1] = F2 importance mode (cache_params.y).
                const float avgSeed = params_.enable_jitter
                    ? static_cast<float>(camera_data.frame_index) : -1.0f;
                const float avgIsMode = params_.enable_importance ? 1.0f : 0.0f;
                AvgCB acb{grid_width_ * grid_height_, params_.rays_per_probe, avgSeed, avgIsMode};
                static_assert(sizeof(AvgCB) == 16);
                // Vulkan binding 2 is an explicit UBO. SetComputeBytes writes
                // push constants there, so it cannot update this descriptor.
                // Regenerating different rays in Average corrupts SH direction
                // and PDF weights even when the trace itself is correct.
                if (auto* cb = device_->MapBuffer(avg_constants_[frameIdx])) {
                    std::memcpy(cb, &acb, sizeof(acb));
                    device_->UnmapBuffer(avg_constants_[frameIdx]);
                }

                DescriptorData params[] = {
                    {0, DescriptorType::StorageBuffer, probe_radiance_buffer_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},
                    {2, DescriptorType::UniformBuffer, avg_constants_[frameIdx]},
                    {3, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                    {4, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},  // F2
                };
                UpdateDescriptorSet(device_, avg_ds_[frameIdx], params, 5);

                cmd->BindComputePipeline(avg_pipeline_);
                const DescriptorSetHandle sets[] = { avg_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, avg_layout_, 0, 1, sets, 0, nullptr);
                if (!splitIsVk)
                    cmd->SetComputeBytes(2, &acb, sizeof(AvgCB));

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch(totalProbes, 1, 1);
                SpgiStageMark(frameIdx, "avg");
            }

            // Barrier: avg radiance written
            {
                ResourceBarrier barrier{};
                barrier.resource = probe_avg_radiance_[frameIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 2.75: Probe Temporal Accumulation (T5.2 reprojection)
            // ==================================================================
            ResourceHandle filteredSH = probe_avg_radiance_[frameIdx];
            if (temporal_pipeline_ != handles::INVALID_PIPELINE &&
                !SpgiStageSkipped("post") && !SpgiStageSkipped("temporal")) {
                // std140 layout: mat4 prevVP (64B) + 10 scalars (48B) = 112B
                struct alignas(16) TemporalCB {
                    math::m4x4 prev_view_projection;
                    u32 totalProbes; float alpha; float posThreshold; float normalThreshold;
                    float clampScale; float maxGradient; u32 gridW; u32 gridH;
                    float mode; float pad_;
                };
                // F3 (M7): strategy → shader mode. EMA=0 (default, behavior
                // unchanged), EMAVarianceClamp=1 (+ neighborhood AABB clamp),
                // Reprojection=2 (pure bilinear history reuse, no clamps).
                float temporalMode = 0.0f;
                switch (temporal_strategy_) {
                    case GITemporalStrategy::EMAVarianceClamp: temporalMode = 1.0f; break;
                    case GITemporalStrategy::Reprojection:     temporalMode = 2.0f; break;
                    default: break;
                }
                // A11: one-shot reset — alpha 1.0 keeps only the current
                // trace, dropping any stale history in one pass.
                const float temporalAlpha = temporal_reset_pending_ ? 1.0f : 0.1f;
                temporal_reset_pending_ = false;
                TemporalCB tcb{
                    camera_data.prev_proj_matrix * camera_data.prev_view_matrix,
                    grid_width_ * grid_height_, temporalAlpha, 0.5f, 0.5f, 2.0f, 2.0f,
                    grid_width_, grid_height_, temporalMode, 0.0f};

                // Vulkan reads the pool UBO (binding 5); Metal uses setBytes below.
                {
                    auto* cb = static_cast<TemporalCB*>(device_->MapBuffer(temporal_cb_[frameIdx]));
                    if (cb) { *cb = tcb; device_->UnmapBuffer(temporal_cb_[frameIdx]); }
                }

                DescriptorData params[] = {
                    {0, DescriptorType::StorageBuffer, probe_avg_radiance_[frameIdx]},
                    {1, DescriptorType::StorageBuffer, probe_avg_radiance_[histIdx]},
                    {2, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                    {3, DescriptorType::StorageBuffer, probe_positions_buffer_[histIdx]},
                    {4, DescriptorType::StorageBuffer, probe_temporal_radiance_[frameIdx]},
                    {5, DescriptorType::UniformBuffer, temporal_cb_[frameIdx]},
                    {6, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                    {7, DescriptorType::StorageBuffer, probe_normals_buffer_[histIdx]},
                };
                UpdateDescriptorSet(device_, temporal_ds_[frameIdx], params, 8);

                cmd->BindComputePipeline(temporal_pipeline_);
                const DescriptorSetHandle sets[] = { temporal_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, temporal_layout_, 0, 1, sets, 0, nullptr);
                if (!splitIsVk) cmd->SetComputeBytes(5, &tcb, sizeof(TemporalCB));

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch((totalProbes + 63) / 64, 1, 1);
                filteredSH = probe_temporal_radiance_[frameIdx];
                SpgiStageMark(frameIdx, "temporal");
            }

            // Barrier: temporal output written
            {
                ResourceBarrier barrier{};
                barrier.resource = filteredSH;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 2.9: Probe Spatial Filter
            // ==================================================================
            if (spatial_pipeline_ != handles::INVALID_PIPELINE &&
                !SpgiStageSkipped("post") && !SpgiStageSkipped("spatial")) {
                struct SpatialCB { u32 totalProbes; u32 gridW; float sigma; float pad; };
                const ResourceHandle spatialOutput = filteredSH == probe_avg_radiance_[frameIdx]
                    ? probe_temporal_radiance_[frameIdx] : probe_avg_radiance_[frameIdx];
                SpatialCB scb{grid_width_ * grid_height_, grid_width_, 2.0f, 0.0f};
                static_assert(sizeof(SpatialCB) == 16);
                if (auto* cb = device_->MapBuffer(spatial_cb_[frameIdx])) {
                    std::memcpy(cb, &scb, sizeof(scb));
                    device_->UnmapBuffer(spatial_cb_[frameIdx]);
                }

                DescriptorData params[] = {
                    {0, DescriptorType::StorageBuffer, filteredSH},
                    {1, DescriptorType::StorageBuffer, spatialOutput},
                    {2, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                    {3, DescriptorType::UniformBuffer, spatial_cb_[frameIdx]},
                };
                UpdateDescriptorSet(device_, spatial_ds_[frameIdx], params, 4);

                cmd->BindComputePipeline(spatial_pipeline_);
                const DescriptorSetHandle sets[] = { spatial_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, spatial_layout_, 0, 1, sets, 0, nullptr);
                if (!splitIsVk) cmd->SetComputeBytes(3, &scb, sizeof(SpatialCB));

                u32 totalProbes = grid_width_ * grid_height_;
                cmd->Dispatch((totalProbes + 63) / 64, 1, 1);
                filteredSH = spatialOutput;
                SpgiStageMark(frameIdx, "spatial");
            }

            // If either optional filter is unavailable, preserve its last valid
            // output in the current history slot too (never overwrite histIdx).
            // A targeted `spatial` skip still goes through this copy so the
            // next frame observes the same temporal history chain as the
            // default path; Gather receives the copied temporal result.
            if (filteredSH != probe_avg_radiance_[frameIdx]) {
                ResourceBarrier copies[2]{};
                copies[0].resource = filteredSH;
                copies[0].beforeState = ResourceState::UnorderedAccess;
                copies[0].afterState = ResourceState::CopySource;
                copies[0].subresource = 0xFFFFFFFF;
                copies[1].resource = probe_avg_radiance_[frameIdx];
                copies[1].beforeState = ResourceState::ShaderResource;
                copies[1].afterState = ResourceState::CopyDest;
                copies[1].subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(copies, 2);
                cmd->CopyBuffer(filteredSH, probe_avg_radiance_[frameIdx], 0, 0,
                                static_cast<u64>(grid_width_) * grid_height_ * 4 * sizeof(math::v4));
                copies[1].beforeState = ResourceState::CopyDest;
                copies[1].afterState = ResourceState::ShaderResource;
                cmd->InsertBarrier(&copies[1], 1);
                filteredSH = probe_avg_radiance_[frameIdx];
            }

            // Barrier: spatial output written; previous-frame SH stays read-only.
            {
                ResourceBarrier barrier{};
                barrier.resource = filteredSH;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 3: ScreenProbeGather
            // ==================================================================
            if (!SpgiStageSkipped("gather")) {
                // Vulkan: flat bindings (images 0-2, SSBO 3/4/6, UBO 5) matching
                // ScreenProbeGather.comp; Metal: texture(0,1,2) + buffer(0,2,4)
                // with setBytes(3).
                DescriptorData params[7];
                if (splitIsVk) {
                    DescriptorData p[] = {
                        {0, DescriptorType::SampledImage,  depthTex},
                        {1, DescriptorType::SampledImage,  normalTex},
                        {2, DescriptorType::StorageImage,  outputTex},
                        {3, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                        {4, DescriptorType::StorageBuffer, filteredSH},
                        {5, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                        {6, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                    };
                    memcpy(params, p, sizeof(p));
                } else {
                    DescriptorData p[] = {
                        {0, DescriptorType::SampledImage,  depthTex},
                        {1, DescriptorType::SampledImage,  normalTex},
                        {2, DescriptorType::StorageImage,  outputTex},
                        {0, DescriptorType::StorageBuffer, probe_positions_buffer_[frameIdx]},
                        {2, DescriptorType::StorageBuffer, filteredSH},
                        {3, DescriptorType::UniformBuffer, global_cb_[frameIdx]},
                        {4, DescriptorType::StorageBuffer, probe_normals_buffer_[frameIdx]},
                    };
                    memcpy(params, p, sizeof(p));
                }
                UpdateDescriptorSet(device_, gather_ds_[frameIdx], params, 7);

                EnsureStorageWritable(outputTex);
                cmd->BindComputePipeline(gather_pipeline_);
                const DescriptorSetHandle sets[] = { gather_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, gather_layout_, 0, 1, sets, 0, nullptr);
                if (!splitIsVk) cmd->SetComputeBytes(3, &gd, sizeof(ScreenProbeGlobalData));

                u32 dispatchX = (render_width_ + 7) / 8;
                u32 dispatchY = (render_height_ + 7) / 8;
                cmd->Dispatch(dispatchX, dispatchY, 1);
                SpgiStageMark(frameIdx, "gather");
            }

            // Barrier: Gather output written
            {
                ResourceBarrier barrier{};
                barrier.resource = outputTex;
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }

            // ==================================================================
            // Sub-pass 4: ScreenProbeDenoise
            // ==================================================================
            if (denoise_pipeline_ != handles::INVALID_PIPELINE &&
                !SpgiStageSkipped("post") &&
                !SpgiStageSkipped("denoise") &&
                filteredTex != handles::INVALID_RESOURCE) {
                struct DenoiseCB {
                    float sigma_depth; float sigma_normal; float sigma_spatial; float pad1;
                    u32 kernel_radius; u32 render_width; u32 render_height; u32 pad2;
                };
                DenoiseCB dcb{8.0f, 8.0f, 1.2f, 0.0f, 1, render_width_, render_height_, 0};

                // T5.3: Vulkan flat DSL puts depth at StorageImage(2, readonly)
                // and params at UBO(4) — the previous single Metal-style write
                // clashed depth/params on binding 0 and never bound the UBO.
                if (splitIsVk) {
                    DescriptorData params[] = {
                        {0, DescriptorType::SampledImage,  outputTex},
                        {1, DescriptorType::SampledImage,  normalTex},
                        {2, DescriptorType::SampledDepthImage, depthTex},
                        {3, DescriptorType::StorageImage,  filteredTex},
                        {4, DescriptorType::UniformBuffer, denoise_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, denoise_ds_[frameIdx], params, 5);
                    auto* cb = static_cast<DenoiseCB*>(device_->MapBuffer(denoise_cb_[frameIdx]));
                    if (cb) { *cb = dcb; device_->UnmapBuffer(denoise_cb_[frameIdx]); }
                } else {
                    DescriptorData params[] = {
                        {0, DescriptorType::SampledImage,  outputTex},
                        {1, DescriptorType::SampledImage,  normalTex},
                        {2, DescriptorType::SampledImage,  depthTex},
                        {3, DescriptorType::StorageImage,  filteredTex},
                        {0, DescriptorType::UniformBuffer, denoise_cb_[frameIdx]},
                    };
                    UpdateDescriptorSet(device_, denoise_ds_[frameIdx], params, 5);
                }

                EnsureStorageWritable(filteredTex);
                cmd->BindComputePipeline(denoise_pipeline_);
                const DescriptorSetHandle sets[] = { denoise_ds_[frameIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, denoise_layout_, 0, 1, sets, 0, nullptr);
                if (!splitIsVk) cmd->SetComputeBytes(0, &dcb, sizeof(DenoiseCB));

                u32 dispatchX = (render_width_ + 7) / 8;
                u32 dispatchY = (render_height_ + 7) / 8;
                cmd->Dispatch(dispatchX, dispatchY, 1);
                SpgiStageMark(frameIdx, "denoise");
            }
        }
    );

    return output;
}

} // namespace primal::graphics::lumen
