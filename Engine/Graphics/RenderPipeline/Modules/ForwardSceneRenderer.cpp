#include "ForwardSceneRenderer.h"
#include "ParticleFX/core/PfxSystem.h"
#include "ParticleFX/render/Bridges.h"
#include "../../VirtualTexture/VirtualTextureSystem.h"
#include "Graphics/RHI/Core/RHIMath.h"
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
#include "Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Graphics/Dawn/ShaderLoader.h"
#endif
#include "Graphics/RHI/Core/RHIShaderCommon.h"  // T4.6.5 part 6: GlobalShaderData + ForwardLightBuffer
#if defined(__APPLE__)
// Metal 平台头仅 macOS 编译（Windows/Linux 下 MTL 符号不存在；此前守卫只排除了
// Emscripten，导致 MSVC 把 Metal 头拉进编译单元）
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalTexture.h"
#include "Graphics/RHI/Platforms/Metal/MetalPipeline.h"
#endif
#include <chrono>
#include "Graphics/RenderScene.h"
#include "Graphics/RenderPipeline/StreamingMesh.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Utils/IBLPrecomputer.h"
#include "Graphics/Scene/LightSyncSystem.h"
#include "Graphics/DebugDraw/DebugDrawQueue.h"
#include "Content/ContentToEngine.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Geometry.h"
#include "Graphics/Material/ShaderTechnique.h"
#include "Graphics/Material/InstanceData.h"
#include "Graphics/MaterialGraph/MaterialGraph.h"
#include "Graphics/MaterialGraph/MaterialGraphSerializer.h"
#include "Graphics/ShaderIR/ShaderCompiler.h"
#include "Graphics/ShaderIR/MaterialGraphToIR.h"
#include "Graphics/ShaderIR/MetalEmitter.h"

#include <fstream>
#include <sstream>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <set>
#include <unordered_map>
#include <array>
#include <filesystem>

// For texture loading
#include "stb_image.h"
#include <zlib.h>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;
using namespace rendergraph;

namespace {

// GPU buffer structs — must match shader layout
struct ViewData {
    math::m4x4 viewProjection;
    math::m4x4 invViewProjection;
    math::m4x4 previousViewProjection;
};

struct SceneData {
    math::m4x4 model;
    math::v4 lightPos;
    math::v4 lightColor;
    math::v4 reflectionPlane;
    math::v4 reflectionPlane2;
    math::v4 reflectionPlane3;
    math::m4x4 previousModel;
    math::v2 jitter;
    math::v2 previousJitter;
    float  time;
    float  _timePad;
    math::v4 viewPos;
    math::m4x4 shadowMatrix0;
    math::m4x4 shadowMatrix1;
    math::m4x4 shadowMatrix2;   // P4.2: 级联 2（对齐 DeferredLighting.metal 544B）
    math::m4x4 shadowMatrix3;   // P4.2: 级联 3
    math::v4 shadowParams;
    math::v4 ambientLight;
};
static_assert(sizeof(SceneData) == 544, "SceneData must match DeferredLighting.metal P4.2 544B layout");

// RG pass data structs
struct ShadowPassData { RGResourceHandle shadowMap; };
struct MainPassData { RGResourceHandle albedo, normal, orm, velocity, depth; };
struct LightingPassData {
    RGResourceHandle output, albedo, normal, orm, depth, shadowMap0, shadowMap1;
};
struct CubemapPassData { RGResourceHandle output, depth; };
struct BlitData { RGResourceHandle input, output; };

// Descriptor update helper
struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    u32 count = 1;
};

static void UpdateDesc(RHIDeviceBase* device, DescriptorSetHandle set,
                       const DescData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (u32 i = 0; i < count; ++i) {
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
        } else if (params[i].type == DescriptorType::Sampler) {
            imageInfos[i].sampler = static_cast<SamplerHandle>(params[i].resource);
            writes[i].imageInfo = &imageInfos[i];
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// T4.6.5 part 16.2: shader dir paths are now relative (worktree-portable).
// Test cwd is `build/Tests/UnitTests/`; CMake POST_BUILD copies Engine/ tree
// next to the binary. Absolute path used as fallback for dev-machine runs
// where the POST_BUILD copy hasn't populated yet.
static const std::string SHADER_DIR =
    "Engine/Graphics/Metal/shaders/Forward/";

static const std::string RHI_SHADER_DIR =
    "Engine/Graphics/RHI/Shaders/";

static const std::string VULKAN_SHADER_DIR =
    "Engine/Graphics/Vulkan/shaders/Forward/";

static const std::string VULKAN_COMPUTE_SHADER_DIR =
    "Engine/Graphics/Vulkan/shaders/";

static const char* SHADER_FALLBACK_ROOT =
    "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/";

// T4.6.5 part 16.2: try `relPath` first (matches POST_BUILD copy layout);
// fall back to `<dev-machine-root>/<relPath>` if the test binary is run from
// a cwd where the copy hasn't been done (e.g. ad-hoc dev runs).
static std::string ResolveShaderPath(const std::string& relPath) {
    if (std::ifstream(relPath).is_open()) return relPath;
    std::string fb = std::string(SHADER_FALLBACK_ROOT) + relPath;
    if (std::ifstream(fb).is_open()) return fb;
    return relPath;  // let the caller's open() produce the error
}

static const std::string SHADER_DIR_WT =
    "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/Engine/Graphics/Metal/shaders/Forward/";

static const std::string RHI_SHADER_DIR_WT =
    "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/Engine/Graphics/RHI/Shaders/";

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Binary file reader for precompiled SPIR-V. Returns empty vector on failure.
static std::vector<u8> ReadFileToBytes(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) return {};
    const std::streamsize size = file.tellg();
    if (size <= 0) return {};
    file.seekg(0, std::ios::beg);
    std::vector<u8> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

static std::string ResolveInclude(const std::string& name) {
    // Search Forward/ then RHI/Shaders/
    for (const auto& dir : {SHADER_DIR, RHI_SHADER_DIR}) {
        auto content = ReadFileToString(ResolveShaderPath(dir + name));
        if (!content.empty()) return content;
    }
    return {};
}

static std::string InlineIncludes(const std::string& source, int depth,
                                   std::set<std::string>& included) {
    if (depth > 8) return source;

    const std::string prefix = "#include \"";
    const std::string suffix = "\"";
    std::string result;
    size_t pos = 0;

    while (pos < source.size()) {
        size_t includePos = source.find(prefix, pos);
        if (includePos == std::string::npos) {
            result += source.substr(pos);
            break;
        }
        size_t nameStart = includePos + prefix.size();
        size_t nameEnd = source.find(suffix, nameStart);
        if (nameEnd == std::string::npos) {
            result += source.substr(pos);
            break;
        }
        std::string includeName = source.substr(nameStart, nameEnd - nameStart);
        result += source.substr(pos, includePos - pos);

        if (included.count(includeName)) {
            // Already included — skip (#pragma once equivalent)
        } else {
            std::string includeContent = ResolveInclude(includeName);
            if (!includeContent.empty()) {
                included.insert(includeName);
                result += InlineIncludes(includeContent, depth + 1, included);
            } else {
                result += prefix + includeName + suffix;
            }
        }

        pos = nameEnd + suffix.size();
    }
    return result;
}

// T4.6.5: LoadShaderSource is platform-aware.
//   Metal: text-mode .metal with #include inlining. Entry-point name passed
//          through to CreateShader as-is.
//   Vulkan: binary .spv with stage suffix (e.g. "Skybox.vert.spv",
//           "Skybox.frag.spv"). Single entry point per file named "main".
//
// Note: full Vulkan activation is blocked beyond just file existence — the
// existing SPIR-V ports use different stage models (e.g. DeferredLighting.spv
// is a GLCompute shader, but the Metal path loads vertexMain/fragmentLighting_v3
// as vertex+fragment). Removing the T4.6.3 skip requires aligning stage models,
// not just adding more .spv files. See plan T4.6.5 for the full blocker list.
static std::vector<u8> LoadShaderSource(const char* filename, RHIPlatform platform,
                                        ShaderStage stage) {
    if (platform == RHIPlatform::Vulkan) {
        // Convention: <Name>.<stage>.spv with entry point "main"
        // Compute path: existing DeferredLighting.spv lives in the parent
        // shaders/ dir (not Forward/), with no stage suffix (single .spv file).
        // Caller passes "DeferredLighting" + ShaderStage::Compute to hit that path.
        if (stage == ShaderStage::Compute) {
            std::string relPath = VULKAN_COMPUTE_SHADER_DIR + filename + ".spv";
            std::string path = ResolveShaderPath(relPath);
            auto bytes = ReadFileToBytes(path);
            if (bytes.empty()) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to load compute SPIR-V: " << path << std::endl;
                return {};
            }
            return bytes;
        }
        const char* stageSuffix = (stage == ShaderStage::Vertex) ? ".vert" : ".frag";
        std::string relPath = VULKAN_SHADER_DIR + filename + stageSuffix + ".spv";
        std::string path = ResolveShaderPath(relPath);
        auto bytes = ReadFileToBytes(path);
        if (bytes.empty()) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to load SPIR-V: " << path << std::endl;
            return {};
        }
        return bytes;
    }

    std::string path = ResolveShaderPath(SHADER_DIR + filename + std::string(".metal"));
    std::string source = ReadFileToString(path);
    if (source.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to load shader: " << filename << std::endl;
        return {};
    }
    std::set<std::string> included;
    source = InlineIncludes(source, 0, included);

    return std::vector<u8>(source.begin(), source.end());
}

static void WriteTextureImmediate(RHIDeviceBase* device, ResourceHandle texture,
                                   const void* data, u64 size, u32 w, u32 h, u32 layer) {
#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU
    // Dawn path: wgpuQueueWriteTexture handles 256-byte row padding internally.
    // The generic staging-buffer + CopyBufferToTexture path below requires
    // bufferRowLength to be a multiple of 256, but every caller passes 0
    // (= "tightly packed"). For 1x1 RGBA8 fallbacks (4 bytes/row) WebGPU
    // rejects the copy outright — "byte size of each row (4) > bytesPerRow (0)".
    auto* dawnDev = dynamic_cast<DawnDevice*>(device);
    if (dawnDev) {
        // Derive row pitch from size & height. Layer is the cube face index;
        // the staging data is a single face so size = rowPitch * h.
        u32 rowPitch = (h > 0) ? static_cast<u32>(size / h) : static_cast<u32>(size);
        dawnDev->UpdateTextureData(texture, data,
                                   0, 0, layer, w, h, 1, rowPitch, 0);
        return;
    }
#endif

    BufferDesc stagingDesc{};
    stagingDesc.size = size;
    stagingDesc.usage = GPUMemoryUsage::Dynamic;
    stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ResourceHandle staging = device->CreateBuffer(stagingDesc);
    if (staging == handles::INVALID_RESOURCE) return;

    void* mapped = device->MapBuffer(staging);
    if (mapped) {
        memcpy(mapped, data, size);
        device->UnmapBuffer(staging);
    }

    SyncHandle fence = device->CreateSync();
    auto cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* cmd = GetCommandBuffer(cmdHandle);
    cmd->Begin();

    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.imageSubresource.baseArrayLayer = layer;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};
    cmd->CopyBufferToTexture(staging, texture, &region, 1);
    cmd->End();

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdHandle;
    submitInfo.signalFence = fence;
    device->Submit(submitInfo);
    device->WaitForSync(fence, UINT32_MAX);

    device->DestroySync(fence);
    device->DestroyCommandBuffer(cmdHandle);
    device->DestroyBuffer(staging);
}

// Texture loading — uses RHI device directly (avoids content system dependency)
ResourceHandle LoadTextureFromFile(RHIDeviceBase* dev, const std::string& path, bool isSRGB) {
    int width, height, channels;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] stbi_load failed: " << path << std::endl;
        return handles::INVALID_RESOURCE;
    }

    DataFormat format = isSRGB ? DataFormat::RGBA8_sRGB : DataFormat::RGBA8_UNorm;
    TextureDesc desc{};
    desc.size = {(u32)width, (u32)height, 1};
    desc.format = format;
    desc.type = TextureType::Texture2D;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    desc.memoryUsage = GPUMemoryUsage::Static;

    ResourceHandle tex = dev->CreateTexture(desc);
    if (tex == handles::INVALID_RESOURCE) {
        stbi_image_free(data);
        return handles::INVALID_RESOURCE;
    }

    u64 dataSize = (u64)width * height * 4;
    WriteTextureImmediate(dev, tex, data, dataSize, width, height, 0);
    stbi_image_free(data);
    return tex;
}

// Resolves a texture reference from the cooked .mesh material header
// against candidate roots. References are stored as recorded at import
// time (bare filenames or source-relative paths). extraSubdir is the
// per-asset texture folder the editor importer copies referenced files
// into ("<stem>_textures/" next to the .mesh); the fbx_textures /
// models/Sponza probes keep the EngineTest asset layout working.
std::string ResolveTexturePath(const std::string& baseDir, const std::string& filename,
                               const std::string& extraSubdir = {}) {
    if (filename.empty()) return "";
    std::string cleanName = filename;
    std::replace(cleanName.begin(), cleanName.end(), '\\', '/');

    std::vector<std::string> basePaths = {
        baseDir,
        baseDir + "fbx_textures/",
        baseDir + "models/Sponza/",
    };
    if (!extraSubdir.empty()) {
        basePaths.insert(basePaths.begin() + 1, baseDir + extraSubdir);
    }
    for (const auto& base : basePaths) {
        std::string fullPath = base + cleanName;
        std::ifstream f(fullPath.c_str());
        if (f.good()) return fullPath;
    }
    return baseDir + cleanName;
}

// f32 → f16 for RGBA16_Float texture upload (HDR equirect).
static inline u16 f32_to_f16(f32 f) {
    u32 x;
    std::memcpy(&x, &f, sizeof(x));
    const u32 sign = (x >> 16) & 0x8000;
    const int e = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    const u32 m = x & 0x7FFFFF;
    if (e <= 0) return static_cast<u16>(sign);
    if (e >= 31) return static_cast<u16>(sign | 0x7C00);
    return static_cast<u16>(sign | (e << 10) | (m >> 13));
}

// Parse a cooked .texture (TEXR container, Apple layout — same encoding as
// ContentToEngine.cpp create_texture_resource) and return mip 0 as RGBA16
// (f16) pixels for equirect upload. Bytes-per-pixel is derived from
// row_pitch/width (4 = RGBA8, 8 = RGBA16, 16 = RGBA32F); the container's
// format field is unreliable for editor-imported HDR files.
static bool LoadTexrEquirect(const std::string& path, u32& out_w, u32& out_h,
                             DataFormat& out_format, std::vector<u8>& out_pixels) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<u8> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 64 || data[0] != 'T' || data[1] != 'E' || data[2] != 'X' || data[3] != 'R') return false;

    u64 off = 8;
    u32 srcLen = 0;
    std::memcpy(&srcLen, data.data() + off, 4); off += 4 + srcLen;
    off += 7 * 4;
    if (off + 28 > data.size()) return false;

    u32 w = 0, h = 0, arraySz = 0, flags = 0, mips = 0, format = 0;
    std::memcpy(&w, data.data() + off, 4);
    std::memcpy(&h, data.data() + off + 4, 4);
    std::memcpy(&arraySz, data.data() + off + 8, 4);
    std::memcpy(&flags, data.data() + off + 12, 4);
    std::memcpy(&mips, data.data() + off + 16, 4);
    std::memcpy(&format, data.data() + off + 20, 4);
    off += 24;
    u32 compLen = 0;
    std::memcpy(&compLen, data.data() + off, 4); off += 4;
    if (arraySz == 0 || mips == 0 || compLen == 0 || off + compLen > data.size()) return false;

    uLongf decSize = (data.size() * 8 + 1) << 4; // generous upper bound
    std::vector<u8> dec(decSize);
    if (uncompress(dec.data(), &decSize, data.data() + off, compLen) != Z_OK) return false;
    dec.resize(decSize);

    // Walk slices to mip 0 / face 0: per slice {u64 skip, u32 row_pitch,
    // u32 slice_pitch, bytes[slice_pitch]}.
    u64 pos = 0;
    auto read_u32 = [&](u32& v) { if (pos + 4 > dec.size()) return false; std::memcpy(&v, dec.data() + pos, 4); pos += 4; return true; };
    for (u32 a = 0; a < arraySz; ++a) {
        for (u32 m = 0; m < mips; ++m) {
            u32 rp = 0, sp = 0;
            if (pos + 16 > dec.size()) return false;
            pos += 8; // unknown u64 header per slice
            if (!read_u32(rp) || !read_u32(sp)) return false;
            if (pos + sp > dec.size()) return false;
            if (a == 0 && m == 0) {
                if (w == 0 || h == 0 || rp == 0 || sp == 0) return false;
                out_w = w; out_h = h;
                const u64 blocksX = (w + 3) / 4, blocksY = (h + 3) / 4;
                if (rp == blocksX * 16 && sp == blocksX * blocksY * 16) {
                    // ASTC_4x4 — upload block data as-is, hardware decodes.
                    // sRGB variant: importer output is sRGB-encoded LDR;
                    // sampling decodes to linear so sky + IBL bake agree.
                    out_format = DataFormat::ASTC_4x4_sRGB;
                    out_pixels.assign(dec.data() + pos, dec.data() + pos + sp);
                    return true;
                }
                // Uncompressed rows may be padded by the cooker.  Derive the
                // texel size from the row pitch, but address source pixels by
                // row so that padding never leaks into the next scanline.
                const u64 bpp = rp / w;
                if (bpp != 4 && bpp != 8 && bpp != 16) return false;
                if (sp < rp * h) return false;
                out_format = DataFormat::RGBA16_Float;
                out_pixels.resize((size_t)w * h * 8);
                const u8* src = dec.data() + pos;
                for (u32 y = 0; y < h; ++y) {
                    const u8* srcRow = src + static_cast<size_t>(y) * rp;
                    for (u32 x = 0; x < w; ++x) {
                        const size_t px = static_cast<size_t>(y) * w + x;
                        const u8* srcPixel = srcRow + static_cast<size_t>(x) * bpp;
                        u8* dstPixel = out_pixels.data() + px * 8;
                        if (bpp == 4) {
                            for (int c = 0; c < 4; ++c) {
                                const u16 half = f32_to_f16(srcPixel[c] / 255.0f);
                                std::memcpy(dstPixel + c * 2, &half, sizeof(half));
                            }
                        } else if (bpp == 8) {
                            std::memcpy(dstPixel, srcPixel, 8);
                        } else {
                            for (int c = 0; c < 4; ++c) {
                                f32 v;
                                std::memcpy(&v, srcPixel + c * 4, sizeof(v));
                                const u16 half = f32_to_f16(v);
                                std::memcpy(dstPixel + c * 2, &half, sizeof(half));
                            }
                        }
                    }
                }
                return true;
            }
            pos += sp;
        }
    }
    return false;
}

} // anonymous namespace

// ============================================================================
// ForwardSceneRenderer Implementation
// ============================================================================

ForwardSceneRenderer::ForwardSceneRenderer() = default;
ForwardSceneRenderer::~ForwardSceneRenderer() { Shutdown(); }

bool ForwardSceneRenderer::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height,
                                       DataFormat backbuffer_format) {
    if (initialized_) return true;

    // T4.6.5 complete: all 11 ForwardSceneRenderer shaders ported to SPIR-V
    // (parts 1-14), runtime wiring done (parts 15.1-15.4: instance buffer,
    // shadow VP, SoA streaming all wired via Vulkan descriptor writes),
    // Initialize() skip lifted (part 15.5), Editor render smoke passing
    // (part 15.6). Runs end-to-end on Vulkan via StandardRenderPipeline.
    if (device && device->GetPlatform() == RHIPlatform::Vulkan) {
        PRIMAL_LOG_STREAM(Info, "Render") << "[ForwardSceneRenderer] Initializing on Vulkan (T4.6.5 parts 1-15 complete)"
                  << std::endl;
    }

    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    blit_target_format_ = backbuffer_format;

    CreateSamplers();
    CreateDescriptorLayouts();
    CreateShaders();
    CreatePipelines();
    CreatePersistentResources();

    // Create triple-buffered CBs
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;

        desc.size = sizeof(ViewData);
        view_cb_[i] = device_->CreateBuffer(desc);

        desc.size = sizeof(SceneData);
        scene_cb_[i] = device_->CreateBuffer(desc);

        desc.size = sizeof(rhi::ForwardLightBuffer);
        light_cb_[i] = device_->CreateBuffer(desc);
    }

    // Shadow VP constant buffers (one per cascade)
    {
        BufferDesc desc{};
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.size = sizeof(ViewData);
        shadow_view_cb_[0] = device_->CreateBuffer(desc);
        shadow_view_cb_[1] = device_->CreateBuffer(desc);
    }

    // Create triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        DescriptorSetDesc desc{};
        desc.layout = global_set_layout_;
        global_ds_[i] = device_->CreateDescriptorSet(desc);

        desc.layout = lighting_set_layout_;
        lighting_ds_[i] = device_->CreateDescriptorSet(desc);

        // T4.6.5 part 4 Path B: Vulkan-only compute descriptor sets.
        if (device_->GetPlatform() == RHIPlatform::Vulkan) {
            desc.layout = lighting_compute_set_layout_;
            lighting_compute_ds_[i] = device_->CreateDescriptorSet(desc);
        }

        desc.layout = skybox_set_layout_;
        skybox_ds_[i] = device_->CreateDescriptorSet(desc);

        desc.layout = blit_set_layout_;
        blit_ds_[i] = device_->CreateDescriptorSet(desc);
    }

    // T4.6.5 part 15.3: Vulkan-only shadow-pass descriptor sets, one per cascade.
    // Bindings are written per-frame in Render() once shadow VP + scene CB are updated.
    if (device_->GetPlatform() == RHIPlatform::Vulkan) {
        for (int c = 0; c < 2; c++) {
            DescriptorSetDesc desc{};
            desc.layout = global_set_layout_;
            shadow_global_ds_[c] = device_->CreateDescriptorSet(desc);
        }
    }

    // Streaming mesh default material DS — created after white_texture_ and
    // flat_normal_texture_ exist (CreatePersistentResources runs below).
    // All streaming meshes share this default (white albedo, flat normal);
    // per-mesh Material on StreamingMeshRecord is deferred (v1).
    {
        DescriptorSetDesc desc{};
        desc.layout = material_set_layout_;
        streaming_material_ds_ = device_->CreateDescriptorSet(desc);

        DescData params[] = {
            {0, DescriptorType::SampledImage, white_texture_},
            {1, DescriptorType::SampledImage, flat_normal_texture_},
            {2, DescriptorType::SampledImage, white_texture_},
            {3, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
        };
        UpdateDesc(device_, streaming_material_ds_, params, 4);
    }

#ifndef DISABLE_PARTICLE_SYSTEM
    particle_pass_.initialize(device);
#ifndef DISABLE_PARTICLE_FX
    particle_fx_light_pass_.initialize(device);   // §7-N6：亮粒子加色灯（软失败无害）
    particle_fx_mesh_pass_.initialize(device);    // §7-N2：mesh 粒子实例绘制（内置立方体）
#endif
#endif

    // Geometry line renderer
    line_renderer_.Initialize(device_);

    initialized_ = true;
    PRIMAL_LOG_STREAM(Info, "Render") << "[ForwardSceneRenderer] Initialized (" << render_width << "x" << render_height << ")" << std::endl;
    return true;
}

void ForwardSceneRenderer::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;

    // 先卸载场景(mesh/content 资产释放;此前 Shutdown 不调 UnloadScene,
    // 直连使用方Shutdown 即泄漏 rhi_mesh_assets 条目 — 退出时 free_list assert)
    if (scene_loaded_) UnloadScene();

    // Geometry line renderer
    line_renderer_.Shutdown();

#ifndef DISABLE_PARTICLE_SYSTEM
    particle_pass_.shutdown();
#ifndef DISABLE_PARTICLE_FX
    particle_fx_light_pass_.shutdown();
    particle_fx_mesh_pass_.shutdown();
#endif
#endif

    // Compiled graph-material variants reference the fallback textures and
    // default sampler below, so release them first.
    PurgeGraphMaterials();
    if (material_graph_vs_ != handles::INVALID_SHADER) { device_->DestroyShader(material_graph_vs_); material_graph_vs_ = handles::INVALID_SHADER; }
    if (graph_material_layout_ != handles::INVALID_PIPELINE_LAYOUT) { device_->DestroyPipelineLayout(graph_material_layout_); graph_material_layout_ = handles::INVALID_PIPELINE_LAYOUT; }
    if (graph_material_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device_->DestroyDescriptorSetLayout(graph_material_set_layout_); graph_material_set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    // VT mesh 路径资源
    for (auto& ds : vt_material_ds_)
        if (ds != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(ds);
    vt_material_ds_.clear();
    material_is_vt_.clear();
    vt_registered_.clear();
    if (vt_material_params_buf_ != handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(vt_material_params_buf_);
        vt_material_params_buf_ = handles::INVALID_RESOURCE;
    }
    vt_mode_ = false;

    // Destroy pipelines
    for (auto* p : {&gbuffer_pipeline_, &gbuffer_vt_pipeline_, &shadow_pipeline_, &lighting_pipeline_,
                    &skybox_pipeline_, &blit_pipeline_, &alphaclip_pipeline_, &unlit_pipeline_,
                    &foliage_pipeline_, &water_pipeline_, &transparent_pipeline_,
                    &forward_water_pipeline_, &forward_transparent_pipeline_,
                    &streaming_pipeline_}) {
        if (*p != handles::INVALID_PIPELINE) { device_->DestroyPipeline(*p); *p = handles::INVALID_PIPELINE; }
    }
    // Destroy layouts
    for (auto* l : {&gbuffer_layout_, &gbuffer_vt_layout_, &shadow_layout_, &lighting_layout_, &lighting_compute_layout_,
                    &skybox_layout_, &blit_layout_}) {
        if (*l != handles::INVALID_PIPELINE_LAYOUT) { device_->DestroyPipelineLayout(*l); *l = handles::INVALID_PIPELINE_LAYOUT; }
    }
    // Destroy descriptor set layouts
    for (auto* d : {&global_set_layout_, &material_set_layout_, &lighting_set_layout_, &lighting_compute_set_layout_,
                    &skybox_set_layout_, &blit_set_layout_}) {
        if (*d != handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device_->DestroyDescriptorSetLayout(*d); *d = handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    }
    // Destroy textures
    for (auto& sm : shadow_map_) {
        if (sm != handles::INVALID_RESOURCE) { device_->DestroyTexture(sm); sm = handles::INVALID_RESOURCE; }
    }
    for (int i = 0; i < 3; i++) {
        for (auto* t : {&gbuffer_albedo_[i], &gbuffer_normal_[i], &gbuffer_orm_[i],
                        &gbuffer_velocity_[i], &gbuffer_depth_[i], &lighting_output_[i]}) {
            if (*t != handles::INVALID_RESOURCE) { device_->DestroyTexture(*t); *t = handles::INVALID_RESOURCE; }
        }
        if (view_cb_[i] != handles::INVALID_RESOURCE) { device_->DestroyBuffer(view_cb_[i]); view_cb_[i] = handles::INVALID_RESOURCE; }
        if (scene_cb_[i] != handles::INVALID_RESOURCE) { device_->DestroyBuffer(scene_cb_[i]); scene_cb_[i] = handles::INVALID_RESOURCE; }
        // T4.6.5 part 6 Path B: Vulkan-only UBOs.
        if (lighting_global_ubos_[i] != handles::INVALID_RESOURCE) {
            device_->UnmapBuffer(lighting_global_ubos_[i]);
            device_->DestroyBuffer(lighting_global_ubos_[i]);
            lighting_global_ubos_[i] = handles::INVALID_RESOURCE;
            lighting_global_mapped_[i] = nullptr;
        }
        if (lighting_light_ubos_[i] != handles::INVALID_RESOURCE) {
            device_->UnmapBuffer(lighting_light_ubos_[i]);
            device_->DestroyBuffer(lighting_light_ubos_[i]);
            lighting_light_ubos_[i] = handles::INVALID_RESOURCE;
            lighting_light_mapped_[i] = nullptr;
        }
        if (light_cb_[i] != handles::INVALID_RESOURCE) { device_->DestroyBuffer(light_cb_[i]); light_cb_[i] = handles::INVALID_RESOURCE; }
    }
    for (int i = 0; i < 2; i++) {
        if (shadow_view_cb_[i] != handles::INVALID_RESOURCE) { device_->DestroyBuffer(shadow_view_cb_[i]); shadow_view_cb_[i] = handles::INVALID_RESOURCE; }
    }
    // Destroy fallback textures
    if (white_texture_ != handles::INVALID_RESOURCE) { device_->DestroyTexture(white_texture_); white_texture_ = handles::INVALID_RESOURCE; }
    if (flat_normal_texture_ != handles::INVALID_RESOURCE) { device_->DestroyTexture(flat_normal_texture_); flat_normal_texture_ = handles::INVALID_RESOURCE; }
    if (black_cube_texture_ != handles::INVALID_RESOURCE) { device_->DestroyTexture(black_cube_texture_); black_cube_texture_ = handles::INVALID_RESOURCE; }
    // Destroy environment/IBL maps (owned when created via SetEnvironmentMap)
    for (auto* t : {&env_cube_, &irradiance_map_, &prefiltered_map_, &brdf_lut_}) {
        if (*t != handles::INVALID_RESOURCE) { device_->DestroyTexture(*t); *t = handles::INVALID_RESOURCE; }
    }
    env_map_path_.clear();
    if (ibl_precomputer_) { ibl_precomputer_->Shutdown(); ibl_precomputer_.reset(); }
    // Destroy samplers
    if (default_sampler_ != handles::INVALID_SAMPLER) { device_->DestroySampler(default_sampler_); default_sampler_ = handles::INVALID_SAMPLER; }
    if (brdf_sampler_ != handles::INVALID_SAMPLER) { device_->DestroySampler(brdf_sampler_); brdf_sampler_ = handles::INVALID_SAMPLER; }

    device_ = nullptr;
}

// ============================================================================
// Initialization sub-methods
// ============================================================================

void ForwardSceneRenderer::CreateSamplers() {
    SamplerDesc linearWrap{
        .minFilter = FilterMode::Linear, .magFilter = FilterMode::Linear, .mipFilter = FilterMode::Linear,
        .addressU = TextureAddressMode::Wrap, .addressV = TextureAddressMode::Wrap, .addressW = TextureAddressMode::Wrap,
        .comparisonFunc = ComparisonFunc::Never,
    };
    default_sampler_ = device_->CreateSampler(linearWrap);

    SamplerDesc linearClamp{
        .minFilter = FilterMode::Linear, .magFilter = FilterMode::Linear,
        .addressU = TextureAddressMode::Clamp, .addressV = TextureAddressMode::Clamp,
        .comparisonFunc = ComparisonFunc::Never,
    };
    brdf_sampler_ = device_->CreateSampler(linearClamp);
}

void ForwardSceneRenderer::CreateDescriptorLayouts() {
    // Global set (ViewData + SceneData)
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel},
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel},
        };
        // T4.6.5 part 13: Vulkan DepthOnly.vert declares instanceData SSBO at
        // set 0 binding 2 for skeletal/PCG instancing. Add StorageBuffer binding
        // on Vulkan only — Metal path uses [[buffer(N)]] auto-binding and
        // doesn't need a descriptor slot. Declaring an extra binding that
        // other shaders (GBuffer.vert) don't use is valid in Vulkan (just
        // unused).
        //
        // T4.6.5 part 15: Vulkan StreamingGBuffer.vert declares three more SoA
        // SSBOs at set 0 bindings 3/4/5 (positions, elements, indices) for
        // manual indexed drawing indirection. Metal path uses BindVertexBuffers
        // (20,3,...) + per-vertex [[buffer(N)]] semantics; Vulkan cannot express
        // `attributes[indices[gl_VertexIndex]]` via vkCmdBindVertexBuffers alone,
        // so we expose the SoA buffers as SSBOs and replicate the indirection
        // in GLSL. RenderStreamingMeshes still calls BindVertexBuffers(20,3,...)
        // on Vulkan (no-op until C++ wires up the SoA descriptor set writes);
        // the streaming_pipeline_ creation succeeds and an early-out in
        // RenderStreamingMeshes skips draws when streaming_material_ds_ is the
        // default white-fallback DS.
        if (device_->GetPlatform() == RHIPlatform::Vulkan ||
            device_->GetPlatform() == RHIPlatform::D3D12) {
            // D3D12 与 Vulkan 同形：spirv-cross 产出的 HLSL 保持同一组 flat
            // SSBO 绑定（set0 b2-b5），仅字节来源（.cso）不同。
            DescriptorSetLayoutBinding vkBindings[] = {
                bindings[0],
                bindings[1],
                {2, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex},
                {3, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex},
                {4, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex},
                {5, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex},
            };
            global_set_layout_ = device_->CreateDescriptorSetLayout({6, vkBindings});
        } else {
            global_set_layout_ = device_->CreateDescriptorSetLayout({2, bindings});
        }
    }
    // Material set (albedo + normal + ORM + sampler)
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {3, DescriptorType::Sampler, 1, ShaderStage::Pixel},
        };
        material_set_layout_ = device_->CreateDescriptorSetLayout({4, bindings});
    }
    // VT M2.14: VT 材质 set — 页表 + 3 层 atlas + 采样器 + 参数 UBO + feedback
    // (GBufferVT.frag set 1 布局;GBufferVT.metal texture(0..3)/sampler(4)/buffer(5,6))
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Pixel},
            {1, DescriptorType::SampledImage,  1, ShaderStage::Pixel},
            {2, DescriptorType::SampledImage,  1, ShaderStage::Pixel},
            {3, DescriptorType::SampledImage,  1, ShaderStage::Pixel},
            {4, DescriptorType::Sampler,       1, ShaderStage::Pixel},
            {5, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel},
            {6, DescriptorType::StorageBuffer, 1, ShaderStage::Pixel},
        };
        material_set_vt_layout_ = device_->CreateDescriptorSetLayout({7, bindings});
    }
    // Lighting set (13 bindings). On Dawn the depth texture (binding 5) AND
    // shadow maps (bindings 6/7) must be SampledDepthImage so WGSL can declare
    // them as `texture_depth_2d` — WebGPU rejects binding a depth-format
    // texture to a regular Float-sampled slot. Irradiance/prefilter (8/9) are
    // cube maps: isCube=true so Dawn creates a CubeView dimension.
    {
        constexpr DescriptorType depthType =
#ifdef __EMSCRIPTEN__
            DescriptorType::SampledDepthImage;
#else
            DescriptorType::SampledImage;
#endif
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel},
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel},
            {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {5, depthType,                   1, ShaderStage::Pixel},
            {6, depthType,                   1, ShaderStage::Pixel},
            {7, depthType,                   1, ShaderStage::Pixel},
            {8, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr, DescriptorBindingFlags::None, DataFormat::RGBA16_Float, false, false, true},
            {9, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr, DescriptorBindingFlags::None, DataFormat::RGBA16_Float, false, false, true},
            {10, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {11, DescriptorType::Sampler, 1, ShaderStage::Pixel},
            {12, DescriptorType::Sampler, 1, ShaderStage::Pixel},
            {13, DescriptorType::StorageBuffer, 1, ShaderStage::Pixel},
        };
        lighting_set_layout_ = device_->CreateDescriptorSetLayout({14, bindings});
    }
    // T4.6.5 part 4 Path B: Vulkan-only compute lighting set (matches existing
    // Engine/Graphics/Vulkan/shaders/DeferredLighting.spv bindings).
    // Metal path keeps the 13-binding lighting_set_layout_ above.
    if (device_->GetPlatform() == RHIPlatform::Vulkan) {
        DescriptorSetLayoutBinding bindings[] = {
            {0,  DescriptorType::SampledImage,   1, ShaderStage::Compute},
            {1,  DescriptorType::SampledImage,   1, ShaderStage::Compute},
            {2,  DescriptorType::SampledImage,   1, ShaderStage::Compute},
            {3,  DescriptorType::SampledImage,   1, ShaderStage::Compute},
            {4,  DescriptorType::SampledImage,   1, ShaderStage::Compute},
            {5,  DescriptorType::SampledImage,   1, ShaderStage::Compute},
            {6,  DescriptorType::SampledImage,   1, ShaderStage::Compute},
            {7,  DescriptorType::SampledImage,   1, ShaderStage::Compute},
            {8,  DescriptorType::Sampler,        1, ShaderStage::Compute},
            {9,  DescriptorType::UniformBuffer,  1, ShaderStage::Compute},
            {10, DescriptorType::UniformBuffer,  1, ShaderStage::Compute},
            {11, DescriptorType::StorageImage,   1, ShaderStage::Compute},
        };
        lighting_compute_set_layout_ = device_->CreateDescriptorSetLayout({12, bindings});
    }
    // Skybox set — binding 2 is a texture_cube, needs isCube=true on Dawn.
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex},
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex},
            {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr, DescriptorBindingFlags::None, DataFormat::RGBA16_Float, false, false, true},
            {3, DescriptorType::Sampler, 1, ShaderStage::Pixel},
        };
        skybox_set_layout_ = device_->CreateDescriptorSetLayout({4, bindings});
    }
    // Blit set
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {1, DescriptorType::Sampler, 1, ShaderStage::Pixel},
        };
        blit_set_layout_ = device_->CreateDescriptorSetLayout({2, bindings});
    }
}

void ForwardSceneRenderer::CreateShaders() {
#ifdef __EMSCRIPTEN__
    // WASM WGSL ports live in Engine/Graphics/Dawn/shaders/ForwardScene/.
    // Technique variants (AlphaClip/Unlit/Foliage/Water/Transparent) and the
    // StreamingGBuffer path are not ported — their handles stay INVALID and
    // the corresponding pipelines become INVALID. Render() only routes
    // standard-technique proxies through the GBuffer pipeline on WASM.
    auto loadWgsl = [this](const char* name, const char* entry, ShaderStage stage) -> ShaderHandle {
        std::string src = dawn::LoadWGSL(name);
        if (src.empty()) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] WASM: failed to load WGSL: "
                      << name << "/" << entry << std::endl;
            return handles::INVALID_SHADER;
        }
        auto handle = device_->CreateShader(src.data(), src.size(), stage, entry);
        if (handle == handles::INVALID_SHADER) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] WASM: failed to compile WGSL: "
                      << name << "/" << entry << std::endl;
        }
        return handle;
    };

    gbuffer_vs_  = loadWgsl("ForwardScene/GBuffer",          "vertexMain",         ShaderStage::Vertex);
    gbuffer_ps_  = loadWgsl("ForwardScene/GBuffer",          "fragmentMain",       ShaderStage::Pixel);
    shadow_vs_   = loadWgsl("ForwardScene/DepthOnly",        "shadow_mapping_vs",  ShaderStage::Vertex);
    lighting_vs_ = loadWgsl("ForwardScene/DeferredLighting", "vertexMain",         ShaderStage::Vertex);
    lighting_ps_ = loadWgsl("ForwardScene/DeferredLighting", "fragmentLighting_v3", ShaderStage::Pixel);
    blit_vs_     = loadWgsl("ForwardScene/Blit",             "vertexMain",         ShaderStage::Vertex);
    blit_ps_     = loadWgsl("ForwardScene/Blit",             "fragmentBlit",       ShaderStage::Pixel);
    skybox_vs_   = loadWgsl("ForwardScene/Skybox",           "vertexSkybox",       ShaderStage::Vertex);
    skybox_ps_   = loadWgsl("ForwardScene/Skybox",           "fragmentSkybox",     ShaderStage::Pixel);
    return;
#else
    const RHIPlatform platform = device_->GetPlatform();
    auto load = [this, platform](const char* file, const char* entry, ShaderStage stage) -> ShaderHandle {
        auto src = LoadShaderSource(file, platform, stage);
        if (src.empty()) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to load shader: " << file << "/" << entry << std::endl;
            return handles::INVALID_SHADER;
        }
        // Vulkan SPIR-V files use "main" entry point by convention; Metal uses
        // named entries. Exception: DeferredLighting.spv is a Dawn WGSL→SPIR-V
        // product (naga) that keeps its original WGSL entry "deferred_lighting_cs".
        const char* useEntry = entry;
        if (platform == RHIPlatform::Vulkan) {
            const bool isLightingCompute =
                (std::strcmp(file, "DeferredLighting") == 0) &&
                (stage == ShaderStage::Compute);
            useEntry = isLightingCompute ? "deferred_lighting_cs" : "main";
        }
        auto handle = device_->CreateShader(src.data(), src.size(), stage, useEntry);
        if (handle == handles::INVALID_SHADER) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to compile shader: " << file << "/" << entry << std::endl;
        }
        return handle;
    };

    gbuffer_vs_ = load("GBuffer", "vertexMain", ShaderStage::Vertex);
    gbuffer_ps_ = load("GBuffer", "fragmentMain", ShaderStage::Pixel);
    // VT M2.14: 变体缺失不阻塞(shader 文件未部署时保持 INVALID,走原管线)
    gbuffer_vt_ps_ = load("GBufferVT", "fragmentMain", ShaderStage::Pixel);
    shadow_vs_ = load("DepthOnly", "shadow_mapping_vs", ShaderStage::Vertex);

    // T4.6.5 part 5 Path B: Vulkan loads single compute shader for lighting
    // (existing Engine/Graphics/Vulkan/shaders/DeferredLighting.spv). The
    // Metal vert/frag path stays untouched.
    if (platform == RHIPlatform::Vulkan) {
        // entry name "deferred_lighting_cs" is rewritten inside `load` for this
        // specific file/stage combination (see load lambda above).
        lighting_cs_ = load("DeferredLighting", "deferred_lighting_cs", ShaderStage::Compute);
    } else {
        lighting_vs_ = load("DeferredLighting", "vertexMain", ShaderStage::Vertex);
        lighting_ps_ = load("DeferredLighting", "fragmentLighting_v3", ShaderStage::Pixel);
    }
    // T4.6.5 part 3: Vulkan uses dedicated Blit shaders (Path A); Metal still
    // uses the multi-entry DeferredLighting.metal fragmentBlit.
    if (platform == RHIPlatform::Vulkan) {
        blit_vs_ = load("Blit", "main", ShaderStage::Vertex);
        blit_ps_ = load("Blit", "main", ShaderStage::Pixel);
    } else {
        blit_vs_ = load("DeferredLighting", "vertexMain", ShaderStage::Vertex);
        blit_ps_ = load("DeferredLighting", "fragmentBlit", ShaderStage::Pixel);
    }

    skybox_vs_ = load("Skybox", "vertexSkybox", ShaderStage::Vertex);
    skybox_ps_ = load("Skybox", "fragmentSkybox", ShaderStage::Pixel);

    // Technique-specific shaders
    alphaclip_vs_ = load("GBufferAlphaClip", "vertexMain", ShaderStage::Vertex);
    alphaclip_ps_ = load("GBufferAlphaClip", "fragmentMain", ShaderStage::Pixel);
    unlit_vs_ = load("GBufferUnlit", "vertexMain", ShaderStage::Vertex);
    unlit_ps_ = load("GBufferUnlit", "fragmentMain", ShaderStage::Pixel);
    foliage_vs_ = load("GBufferFoliage", "vertexMain", ShaderStage::Vertex);
    foliage_ps_ = load("GBufferFoliage", "fragmentMain", ShaderStage::Pixel);
    water_vs_ = load("GBufferWater", "vertexMain", ShaderStage::Vertex);
    water_ps_ = load("GBufferWater", "fragmentMain", ShaderStage::Pixel);
    transparent_vs_ = load("GBufferTransparent", "vertexMain", ShaderStage::Vertex);
    transparent_ps_ = load("GBufferTransparent", "fragmentMain", ShaderStage::Pixel);

    // Forward pass shaders for Water/Transparent (inline PBR, rendered after deferred lighting).
    // T4.6.5 part 14: Metal funnels 4 entry points into one ForwardTransparency.metal; Vulkan
    // splits them into 4 SPIR-V files with a single `main` entry each.
    if (platform == RHIPlatform::Vulkan) {
        forward_water_vs_ = load("ForwardWater", "main", ShaderStage::Vertex);
        forward_water_ps_ = load("ForwardWater", "main", ShaderStage::Pixel);
        forward_transparent_vs_ = load("ForwardTransparent", "main", ShaderStage::Vertex);
        forward_transparent_ps_ = load("ForwardTransparent", "main", ShaderStage::Pixel);
    } else {
        forward_water_vs_ = load("ForwardTransparency", "forwardWaterVS", ShaderStage::Vertex);
        forward_water_ps_ = load("ForwardTransparency", "forwardWaterFS", ShaderStage::Pixel);
        forward_transparent_vs_ = load("ForwardTransparency", "forwardTransparentVS", ShaderStage::Vertex);
        forward_transparent_ps_ = load("ForwardTransparency", "forwardTransparentFS", ShaderStage::Pixel);
    }

    // Streaming mesh shaders (Phase 9.3b Task 12) — SoA vertex pulling
    streaming_vs_ = load("StreamingGBuffer", "streamingVertexMain", ShaderStage::Vertex);
    streaming_ps_ = load("StreamingGBuffer", "streamingFragmentMain", ShaderStage::Pixel);

    // Companion VS for material-graph generated fragments. Pairs with any
    // emitter-produced pixel shader (strict six-member VertexOut contract).
    // Metal-only for now: MaterialGraphVertex.metal is the runtime-compile
    // source; no Vulkan .vert/.spv variant exists, and loading it there would
    // just log a failure every run (graph materials are an editor/Metal path).
    if (platform == RHIPlatform::Metal) {
        material_graph_vs_ = load("MaterialGraphVertex", "vertexMainMaterialGraph", ShaderStage::Vertex);
    }
#endif
}

// Configure vertex layout for geometry-rendering pipelines. Matches GBuffer.metal
// VertexInput struct: position at offset 0 (12B), ColorTSign 12 (4B),
// Normal 16 (4B), Tangent 20 (4B), UV 24 (8B). Stride 32B, bound at slot 20
// by RenderMesh::Draw(cmd, count, 0, 20). Without this, Metal sees no vertex
// attributes and the vertex shader reads garbage → spheres don't transform →
// no depth writes → black viewport.
static void ConfigureGeometryVertexLayout(GraphicsPipelineDesc& desc) {
    desc.vertexAttributes.push_back({0, 20, DataFormat::RGB32_Float, 0});
    desc.vertexAttributes.push_back({1, 20, DataFormat::R32_UInt,    12});
    desc.vertexAttributes.push_back({2, 20, DataFormat::RG16_UInt,   16});
    desc.vertexAttributes.push_back({3, 20, DataFormat::RG16_UInt,   20});
    desc.vertexAttributes.push_back({4, 20, DataFormat::RG32_Float,  24});
    desc.vertexBindings.push_back({20, 32, true});
}

void ForwardSceneRenderer::CreatePipelines() {
    // Helper: WebGPU cannot expose bound vertex buffers as storage, so we use
    // standard @location(N) attributes (5 vertex inputs at slot 0, stride 32B)
    // and instance-rate attributes for ForwardInstanceData at slot 1 (96B stride).
    // Metal keeps the legacy slot-20/3 vertex-pulling pattern.
    auto populateWasmVertexInput = [](GraphicsPipelineDesc& desc) {
#ifdef __EMSCRIPTEN__
        // Vertex attributes (slot 0, 32B stride):
        //   position(0-11) | color_t_sign(12-15) | packed_normal(16-19)
        //   | packed_tangent(20-23) | uv(24-31)
        // Instance attributes (slot 1, 96B stride = sizeof(ForwardInstanceData)):
        //   transform mat4x4 (4 rows) | baseColor vec4 | roughness/metallic/alphaCutoff/pad
        utl::vector<rhi::VertexInputAttribute> attrs(11);
        attrs[0]  = {0,  0, rhi::DataFormat::RGB32_Float, 0};   // position
        attrs[1]  = {1,  0, rhi::DataFormat::R32_UInt,    12};  // color_t_sign
        attrs[2]  = {2,  0, rhi::DataFormat::R32_UInt,    16};  // packed_normal
        attrs[3]  = {3,  0, rhi::DataFormat::R32_UInt,    20};  // packed_tangent
        attrs[4]  = {4,  0, rhi::DataFormat::RG32_Float,  24};  // uv
        attrs[5]  = {5,  1, rhi::DataFormat::RGBA32_Float, 0};  // transform row 0
        attrs[6]  = {6,  1, rhi::DataFormat::RGBA32_Float, 16}; // transform row 1
        attrs[7]  = {7,  1, rhi::DataFormat::RGBA32_Float, 32}; // transform row 2
        attrs[8]  = {8,  1, rhi::DataFormat::RGBA32_Float, 48}; // transform row 3
        attrs[9]  = {9,  1, rhi::DataFormat::RGBA32_Float, 64}; // baseColor
        attrs[10] = {10, 1, rhi::DataFormat::RGBA32_Float, 80}; // roughness/metallic/alphaCutoff/pad
        desc.vertexAttributes = attrs;
        utl::vector<rhi::VertexInputBinding> binds(2);
        binds[0] = {0, 32, true};   // perVertex
        binds[1] = {1, 96, false};  // perInstance
        desc.vertexBindings = binds;
#endif
    };

    // Offset 0 is 4-byte aligned as Vulkan requires (VUID-VkPushConstantRange-
    // offset-00295); Metal ignores the offset parameter (reserved buffer index).
    PushConstantRange modelPush{ShaderStage::Vertex, 0, sizeof(PCGPushConsts)};

    // T4.6.5 part 14: every GBuffer-style vert shader (GBuffer, AlphaClip, Unlit,
    // Foliage, Water, Transparent, ForwardWater, ForwardTransparent) declares
    // the same 5-location vertex input matching the engine VertexInput layout
    // (32-byte stride). Apply the declaration on Vulkan only — Metal relies on
    // [[buffer(N)]] auto-binding from the shader. StreamingGBuffer uses SoA
    // SSBOs (no vertex attributes); Skybox/Blit are procedural (cleared).
    auto applyGBufferVertexInput = [this](GraphicsPipelineDesc& desc) {
        // D3D12 与 Vulkan 同形：spirv-cross HLSL 顶点输入走 TEXCOORDN 语义表，
        // 需要同一份显式 location 声明；Metal 才依赖 [[buffer(N)]] 自动绑定。
        if (device_->GetPlatform() != RHIPlatform::Vulkan &&
            device_->GetPlatform() != RHIPlatform::D3D12) return;
        utl::vector<VertexInputAttribute> attrs(5);
        attrs[0] = {0, 0, DataFormat::RGB32_Float, 0};
        attrs[1] = {1, 0, DataFormat::R32_UInt,     12};
        attrs[2] = {2, 0, DataFormat::RG16_UInt,    16};
        attrs[3] = {3, 0, DataFormat::RG16_UInt,    20};
        attrs[4] = {4, 0, DataFormat::RG32_Float,   24};
        desc.vertexAttributes = attrs;
        utl::vector<VertexInputBinding> binds(1);
        binds[0] = {0, 32, true};
        desc.vertexBindings = binds;
    };

    // GBuffer
    {
        DescriptorSetLayoutHandle sets[] = {global_set_layout_, material_set_layout_};
        gbuffer_layout_ = device_->CreatePipelineLayout({2, sets, 1, &modelPush});

        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_layout_;
        desc.vertexShader = gbuffer_vs_;
        desc.pixelShader = gbuffer_ps_;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        populateWasmVertexInput(desc);

        // T4.6.5 part 12: Vulkan needs explicit vertex input declaration.
        // Mirror ForwardRenderer bypassProd pattern (cpp:347-356): single
        // binding stride=32, 5 attributes. Use RG16_UInt for normal/tangent
        // to match GBuffer.vert's `uvec2` declaration (ForwardRenderer uses
        // R32_UInt because ForwardPBR_Lite reads them as u32; GBuffer.vert
        // declares uvec2 so we use the 2-component RG16_UInt format).
        if (device_->GetPlatform() == RHIPlatform::Vulkan ||
            device_->GetPlatform() == RHIPlatform::D3D12) {
            utl::vector<VertexInputAttribute> attrs(5);
            attrs[0] = {0, 0, DataFormat::RGB32_Float, 0};   // position
            attrs[1] = {1, 0, DataFormat::R32_UInt,     12};  // colorTSign
            attrs[2] = {2, 0, DataFormat::RG16_UInt,    16};  // normal (packed_ushort2)
            attrs[3] = {3, 0, DataFormat::RG16_UInt,    20};  // tangent (packed_ushort2)
            attrs[4] = {4, 0, DataFormat::RG32_Float,   24};  // uv
            desc.vertexAttributes = attrs;
            utl::vector<VertexInputBinding> binds(1);
            binds[0] = {0, 32, true};
            desc.vertexBindings = binds;
        }
        gbuffer_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // VT M2.14: GBuffer 的 VT 采样管线(vertex 复用 gbuffer_vs_,pixel 用 VT 变体)
    if (gbuffer_vt_ps_ != handles::INVALID_SHADER &&
        material_set_vt_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        DescriptorSetLayoutHandle sets[] = {global_set_layout_, material_set_vt_layout_};
        gbuffer_vt_layout_ = device_->CreatePipelineLayout({2, sets, 1, &modelPush});

        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_vt_layout_;
        desc.vertexShader = gbuffer_vs_;
        desc.pixelShader = gbuffer_vt_ps_;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        populateWasmVertexInput(desc);
        if (device_->GetPlatform() == RHIPlatform::Vulkan ||
            device_->GetPlatform() == RHIPlatform::D3D12) {
            utl::vector<VertexInputAttribute> attrs(5);
            attrs[0] = {0, 0, DataFormat::RGB32_Float, 0};   // position
            attrs[1] = {1, 0, DataFormat::R32_UInt,     12};  // colorTSign
            attrs[2] = {2, 0, DataFormat::RG16_UInt,    16};  // normal
            attrs[3] = {3, 0, DataFormat::RG16_UInt,    20};  // tangent
            attrs[4] = {4, 0, DataFormat::RG32_Float,   24};  // uv
            desc.vertexAttributes = attrs;
            utl::vector<VertexInputBinding> binds(1);
            binds[0] = {0, 32, true};
            desc.vertexBindings = binds;
        }
        gbuffer_vt_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }


    // Material-graph variants: same GBuffer render state, but the material
    // set carries kMaxGraphTextures images (bindings 0..7) + sampler at 8 —
    // the binding convention MetalEmitter's world mode emits.
    {
        std::vector<DescriptorSetLayoutBinding> graphBindings;
        graphBindings.reserve(kMaxGraphTextures + 1);
        for (u32 i = 0; i < kMaxGraphTextures; ++i) {
            graphBindings.push_back({i, DescriptorType::SampledImage, 1, ShaderStage::Pixel});
        }
        graphBindings.push_back({kMaxGraphTextures, DescriptorType::Sampler, 1, ShaderStage::Pixel});
        graph_material_set_layout_ =
            device_->CreateDescriptorSetLayout({(u32)graphBindings.size(), graphBindings.data()});

        DescriptorSetLayoutHandle graphSets[] = {global_set_layout_, graph_material_set_layout_};
        // offset 必须 4 对齐（VUID-VkPushConstantRange-offset-00295）；shader 块从 0 开始，
        // Metal 端忽略 offset 参数，两端统一 0。
        PushConstantRange modelPush{ShaderStage::Vertex, 0, sizeof(PCGPushConsts)};
        graph_material_layout_ = device_->CreatePipelineLayout({2, graphSets, 1, &modelPush});
    }

    // Shadow
    {
        DescriptorSetLayoutHandle sets[] = {global_set_layout_, material_set_layout_};
        shadow_layout_ = device_->CreatePipelineLayout({2, sets, 1, &modelPush});

        GraphicsPipelineDesc desc{};
        desc.layout = shadow_layout_;
        desc.vertexShader = shadow_vs_;
        desc.renderTargetCount = 0;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        populateWasmVertexInput(desc);
        // T4.6.5 part 13: DepthOnly.vert reads only `in_position` at location 0.
        // Same 32-byte stride as GBuffer, but only the position attribute is
        // declared (shader doesn't read normal/uv/etc).
        if (device_->GetPlatform() == RHIPlatform::Vulkan) {
            utl::vector<VertexInputAttribute> attrs(1);
            attrs[0] = {0, 0, DataFormat::RGB32_Float, 0};
            desc.vertexAttributes = attrs;
            utl::vector<VertexInputBinding> binds(1);
            binds[0] = {0, 32, true};
            desc.vertexBindings = binds;
        }
        shadow_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Lighting
    if (device_->GetPlatform() == RHIPlatform::Vulkan) {
        // T4.6.5 part 5 Path B: Vulkan uses compute dispatch. Existing .spv
        // declares workgroup_size 8x8x1; threadGroupSize here is informational
        // (actual dispatch happens at bind site with explicit group counts).
        lighting_compute_layout_ = device_->CreatePipelineLayout({1, &lighting_compute_set_layout_});

        ComputePipelineDesc desc{};
        desc.layout = lighting_compute_layout_;
        desc.computeShader = lighting_cs_;
        desc.threadGroupSize = {8, 8, 1};
        lighting_pipeline_ = device_->CreateComputePipeline(desc);
    } else {
        lighting_layout_ = device_->CreatePipelineLayout({1, &lighting_set_layout_});

        GraphicsPipelineDesc desc{};
        desc.layout = lighting_layout_;
        desc.vertexShader = lighting_vs_;
        desc.pixelShader = lighting_ps_;
        desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::Unknown;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        desc.cullMode = CullMode::None;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        lighting_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Skybox
    {
        skybox_layout_ = device_->CreatePipelineLayout({1, &skybox_set_layout_});

        GraphicsPipelineDesc desc{};
        desc.layout = skybox_layout_;
        desc.vertexShader = skybox_vs_;
        desc.pixelShader = skybox_ps_;
        desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = false;
        desc.depthFunc = ComparisonFunc::LessEqual;
        desc.cullMode = CullMode::None;
        skybox_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Blit
    {
        blit_layout_ = device_->CreatePipelineLayout({1, &blit_set_layout_});

        GraphicsPipelineDesc desc{};
        desc.layout = blit_layout_;
        desc.vertexShader = blit_vs_;
        desc.pixelShader = blit_ps_;
        desc.renderTargetFormats[0] = blit_target_format_;
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::Unknown;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        desc.cullMode = CullMode::None;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        blit_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // AlphaClip technique — shares gbuffer layout, different shader
#ifndef __EMSCRIPTEN__
    // Technique variants (AlphaClip/Unlit/Foliage/Water/Transparent/Forward*)
    // are not ported to WASM yet — their Metal shaders don't load on WASM
    // and CreateGraphicsPipeline fails with INVALID_SHADER vertex handles,
    // cascading into device-level validation. Skip creation entirely; the
    // WFC demo uses only standard GBuffer materials.
    {
        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_layout_;
        desc.vertexShader = alphaclip_vs_;
        desc.pixelShader = alphaclip_ps_;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        applyGBufferVertexInput(desc);
        alphaclip_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Unlit technique — shares gbuffer layout, different shader
    {
        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_layout_;
        desc.vertexShader = unlit_vs_;
        desc.pixelShader = unlit_ps_;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        applyGBufferVertexInput(desc);
        unlit_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Foliage technique — two-sided, wind animation, alpha discard
    {
        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_layout_;
        desc.vertexShader = foliage_vs_;
        desc.pixelShader = foliage_ps_;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        applyGBufferVertexInput(desc);
        foliage_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Water technique — alpha blend, no depth write
    {
        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_layout_;
        desc.vertexShader = water_vs_;
        desc.pixelShader = water_ps_;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = false;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        desc.enableBlend = true;
        desc.srcColorBlendFactor = BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
        desc.colorBlendOp = BlendOp::Add;
        desc.srcAlphaBlendFactor = BlendFactor::One;
        desc.dstAlphaBlendFactor = BlendFactor::InvSrcAlpha;
        desc.alphaBlendOp = BlendOp::Add;
        applyGBufferVertexInput(desc);
        water_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Transparent technique — alpha blend, no depth write
    {
        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_layout_;
        desc.vertexShader = transparent_vs_;
        desc.pixelShader = transparent_ps_;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = false;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        desc.enableBlend = true;
        desc.srcColorBlendFactor = BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
        desc.colorBlendOp = BlendOp::Add;
        desc.srcAlphaBlendFactor = BlendFactor::One;
        desc.dstAlphaBlendFactor = BlendFactor::InvSrcAlpha;
        desc.alphaBlendOp = BlendOp::Add;
        applyGBufferVertexInput(desc);
        transparent_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Forward Water — single RT (lighting_output_), blend over deferred result, depth test against GBuffer
    {
        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_layout_;
        desc.vertexShader = forward_water_vs_;
        desc.pixelShader = forward_water_ps_;
        desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = false;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        desc.enableBlend = true;
        desc.srcColorBlendFactor = BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
        desc.colorBlendOp = BlendOp::Add;
        desc.srcAlphaBlendFactor = BlendFactor::One;
        desc.dstAlphaBlendFactor = BlendFactor::InvSrcAlpha;
        desc.alphaBlendOp = BlendOp::Add;
        applyGBufferVertexInput(desc);
        forward_water_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Forward Transparent — same configuration
    {
        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_layout_;
        desc.vertexShader = forward_transparent_vs_;
        desc.pixelShader = forward_transparent_ps_;
        desc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = false;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        desc.enableBlend = true;
        desc.srcColorBlendFactor = BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
        desc.colorBlendOp = BlendOp::Add;
        desc.srcAlphaBlendFactor = BlendFactor::One;
        desc.dstAlphaBlendFactor = BlendFactor::InvSrcAlpha;
        desc.alphaBlendOp = BlendOp::Add;
        applyGBufferVertexInput(desc);
        forward_transparent_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
#endif // !__EMSCRIPTEN__
    // Streaming mesh pipeline — same layout/RTs/depth as GBuffer, different VS/PS
    // (Phase 9.3b Task 12). Reads SoA buffers at slots 20/21 instead of AoS.
#ifndef __EMSCRIPTEN__
    // Streaming mesh pipeline also uses Metal-only shaders on WASM.
    {
        GraphicsPipelineDesc desc{};
        desc.layout = gbuffer_layout_;
        desc.vertexShader = streaming_vs_;
        desc.pixelShader = streaming_ps_;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
        desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
        desc.renderTargetFormats[3] = DataFormat::RG16_Float;
        desc.renderTargetCount = 4;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.cullMode = CullMode::None;
        // T4.6.5 part 14: StreamingGBuffer.vert pulls vertices from SoA SSBOs
        // (positions/elements/indices) at set 0 bindings 3/4/5 using
        // gl_VertexIndex — no vertex attributes or bindings needed.
        if (device_->GetPlatform() == RHIPlatform::Vulkan) {
            desc.vertexAttributes.clear();
            desc.vertexBindings.clear();
        }
        streaming_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
#endif // !__EMSCRIPTEN__
}

void ForwardSceneRenderer::CreatePersistentResources() {
    // Fallback textures
    {
        TextureDesc desc{};
        desc.size = {1, 1, 1};
        desc.format = DataFormat::RGBA8_UNorm;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
        white_texture_ = device_->CreateTexture(desc);
        u32 white = 0xFFFFFFFF;
        WriteTextureImmediate(device_, white_texture_, &white, 4, 1, 1, 0);

        flat_normal_texture_ = device_->CreateTexture(desc);
        u32 normal = 0xFFFF8080;
        WriteTextureImmediate(device_, flat_normal_texture_, &normal, 4, 1, 1, 0);
    }

    // Cube fallback for IBL slots — shader expects texturecube<float> at bindings 8/9.
    // Scene ambient is passed separately in SceneData, so this stays black.
    {
        TextureDesc desc{};
        desc.size = {1, 1, 1};
        desc.format = DataFormat::RGBA16_Float;
        desc.type = TextureType::TextureCube;
        desc.arraySize = 1;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
        desc.name = "FwdBlackCubeFallback";
        black_cube_texture_ = device_->CreateTexture(desc);
        u64 zero = 0; // RGBA16F all-zero = (0,0,0,0)
        for (u32 face = 0; face < 6; face++) {
            WriteTextureImmediate(device_, black_cube_texture_, &zero, 8, 1, 1, face);
        }

    }

    // Shadow maps (shared, not triple-buffered)
    for (auto& sm : shadow_map_) {
        TextureDesc desc{};
        desc.size = {2048, 2048, 1};
        desc.format = DataFormat::D32_Float;
        desc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
        desc.memoryUsage = GPUMemoryUsage::Static;
        sm = device_->CreateTexture(desc);
    }

    // Triple-buffered GBuffer + lighting textures
    // T4.6.5 part 4 Path B: lighting_output_ gains UnorderedAccess usage on
    // Vulkan so the compute DeferredLighting.spv can write HDR output via
    // OpImageStore.
    const RHIPlatform platform = device_->GetPlatform();
    const bool lightingNeedsUAV = (platform == RHIPlatform::Vulkan);
    for (int i = 0; i < 3; i++) {
        auto makeTex = [&](DataFormat fmt, const char* name, bool uav) -> ResourceHandle {
            TextureDesc desc{};
            desc.size = {render_width_, render_height_, 1};
            desc.format = fmt;
            desc.type = TextureType::Texture2D;
            // T4.6.5 part 16.3: D32 depth can't take RenderTarget on Vulkan
            // (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT illegal on D32_SFLOAT).
            // Use DepthStencil which maps to depth-attachment usage. Metal
            // accepts the redundant bit silently; Vulkan validation rejects it.
            const bool isDepth = (fmt == DataFormat::D32_Float);
            TextureUsage baseUsage = isDepth ? TextureUsage::DepthStencil
                                             : TextureUsage::RenderTarget;
            TextureUsage usage = baseUsage | TextureUsage::ShaderResource;
            if (uav) usage = usage | TextureUsage::UnorderedAccess;
            desc.usage = usage;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = name;
            return device_->CreateTexture(desc);
        };
        gbuffer_albedo_[i] = makeTex(DataFormat::BGRA8_UNorm, "FwdAlbedo", false);
        gbuffer_normal_[i] = makeTex(DataFormat::RGBA16_Float, "FwdNormal", false);
        gbuffer_orm_[i] = makeTex(DataFormat::BGRA8_UNorm, "FwdORM", false);
        gbuffer_velocity_[i] = makeTex(DataFormat::RG16_Float, "FwdVelocity", false);
        gbuffer_depth_[i] = makeTex(DataFormat::D32_Float, "FwdDepth", false);
        lighting_output_[i] = makeTex(DataFormat::RGBA16_Float, "FwdLighting", lightingNeedsUAV);

        // T4.6.5 part 6 Path B: Vulkan-only UBOs for compute lighting.
        // Mirror ForwardRenderer.cpp:54-85 pattern (triple-buffered, mapped).
        if (lightingNeedsUAV) {
            BufferDesc globalDesc{};
            globalDesc.size = sizeof(rhi::GlobalShaderData);
            globalDesc.type = BufferType::Constant;
            globalDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            globalDesc.usage = GPUMemoryUsage::Dynamic;
            globalDesc.bindFlags = static_cast<u32>(ResourceUsage::ConstantBuffer);
            lighting_global_ubos_[i] = device_->CreateBuffer(globalDesc);
            lighting_global_mapped_[i] = device_->MapBuffer(lighting_global_ubos_[i], 0,
                                                            sizeof(rhi::GlobalShaderData));

            BufferDesc lightDesc{};
            lightDesc.size = sizeof(rhi::ForwardLightBuffer);
            lightDesc.type = BufferType::Constant;
            lightDesc.memoryUsage = GPUMemoryUsage::Dynamic;
            lightDesc.usage = GPUMemoryUsage::Dynamic;
            lightDesc.bindFlags = static_cast<u32>(ResourceUsage::ConstantBuffer);
            lighting_light_ubos_[i] = device_->CreateBuffer(lightDesc);
            lighting_light_mapped_[i] = device_->MapBuffer(lighting_light_ubos_[i], 0,
                                                           sizeof(rhi::ForwardLightBuffer));
        }
    }
}

// ============================================================================
// Scene Loading
// ============================================================================

bool ForwardSceneRenderer::LoadScene(const std::string& model_path) {
    if (scene_loaded_) UnloadScene();

    std::ifstream file(model_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to open file: " << model_path << std::endl;
        return false;
    }
    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(file_size);
    if (!file.read(buffer.data(), file_size)) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to read file: " << model_path << std::endl;
        return false;
    }
    mesh_infos_ = scene_adapter_.LoadRenderItemData(device_, buffer.data(), (u32)buffer.size());
    if (mesh_infos_.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to load scene: " << model_path << std::endl;
        return false;
    }

    // Create per-mesh material descriptor sets with loaded textures.
    // Textures resolve against the cooked mesh's own directory — this raw-
    // file path (stb decode) serves EngineTest-style assets, where textures
    // live next to the model (fbx_textures probe). Editor project imports
    // bind differently: their default material graph references cooked
    // .texture assets under "<stem>_textures/", consumed at placement time
    // via PipelineImportCookedTexture instead of this loader.
    std::string assetBaseDir = model_path;
    const size_t lastSlash = assetBaseDir.find_last_of("/\\");
    assetBaseDir = (lastSlash == std::string::npos)
        ? std::string("./")
        : assetBaseDir.substr(0, lastSlash + 1);
    std::string modelStem = model_path;
    const size_t lastSegment = model_path.find_last_of("/\\");
    if (lastSegment != std::string::npos) modelStem = model_path.substr(lastSegment + 1);
    const size_t lastDot = modelStem.find_last_of('.');
    if (lastDot != std::string::npos) modelStem = modelStem.substr(0, lastDot);
    const std::string textureSubdir = modelStem + "_textures/";
    std::unordered_map<std::string, ResourceHandle> textureCache;

    for (auto& meshInfo : mesh_infos_) {
        if (!meshInfo.mesh) continue;
        DescriptorSetDesc desc{};
        desc.layout = material_set_layout_;
        auto matSet = device_->CreateDescriptorSet(desc);

        ResourceHandle albedo = white_texture_;
        ResourceHandle normal = flat_normal_texture_;
        ResourceHandle orm = white_texture_;

        // Load albedo texture
        if (!meshInfo.diffuseTexturePath.empty()) {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.diffuseTexturePath, textureSubdir);
            if (!fullPath.empty()) {
                auto it = textureCache.find(fullPath);
                if (it != textureCache.end()) {
                    albedo = it->second;
                } else {
                    albedo = LoadTextureFromFile(device_, fullPath, true);
                    if (albedo != handles::INVALID_RESOURCE) {
                        textureCache[fullPath] = albedo;
                    } else {
                        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to load albedo: "
                                  << fullPath << std::endl;
                        albedo = white_texture_;
                    }
                }
            }
        }

        // Infer normal path from diffuse if not present
        if (meshInfo.normalTexturePath.empty() && !meshInfo.diffuseTexturePath.empty()) {
            const std::string& diff = meshInfo.diffuseTexturePath;
            std::string inferred;
            if (diff.find("_diffuse.") != std::string::npos) {
                inferred = diff;
                inferred.replace(diff.find("_diffuse."), 9, "_normal.");
            } else if (diff.find("_Albedo.") != std::string::npos) {
                inferred = diff;
                inferred.replace(diff.find("_Albedo."), 8, "_Normal.");
            } else if (diff.find("_Diff.") != std::string::npos) {
                inferred = diff;
                inferred.replace(diff.find("_Diff."), 5, "_Normal.");
            }
            if (!inferred.empty()) {
                std::string fullPath = ResolveTexturePath(assetBaseDir, inferred, textureSubdir);
                std::ifstream testFile(fullPath);
                if (testFile.good()) meshInfo.normalTexturePath = inferred;
            }
        }

        // Load normal texture
        if (!meshInfo.normalTexturePath.empty()) {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.normalTexturePath, textureSubdir);
            if (!fullPath.empty()) {
                auto it = textureCache.find(fullPath);
                if (it != textureCache.end()) {
                    normal = it->second;
                } else {
                    normal = LoadTextureFromFile(device_, fullPath, false);
                    if (normal != handles::INVALID_RESOURCE) {
                        textureCache[fullPath] = normal;
                    } else {
                        normal = flat_normal_texture_;
                    }
                }
            }
        }

        // Store loaded textures for reference
        material_textures_.push_back(albedo);
        material_textures_.push_back(normal);
        material_textures_.push_back(orm);

        DescData params[] = {
            {0, DescriptorType::SampledImage, albedo},
            {1, DescriptorType::SampledImage, normal},
            {2, DescriptorType::SampledImage, orm},
            {3, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
        };
        UpdateDesc(device_, matSet, params, 4);
        material_ds_.push_back(matSet);
        // VT M2 mesh 路径:.vtex 齐备时额外建 VT 材质(draw 点据此选管线;
        // 常规 set 保留 — shadow 等 pass 仍用其布局)
        if (vt_mode_) {
            TryCreateVTMaterial(meshInfo, u32(material_ds_.size()) - 1, assetBaseDir);
        }
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[ForwardSceneRenderer] Loaded scene: " << model_path
              << " (" << mesh_infos_.size() << " meshes, " << textureCache.size()
              << " textures)" << std::endl;

    // Build mesh_id_to_index_ map for ECS bridge
    mesh_id_to_index_.clear();
    for (u32 i = 0; i < mesh_infos_.size(); i++) {
        if (mesh_infos_[i].mesh && mesh_infos_[i].mesh->GetEntityId() != id::invalid_id) {
            mesh_id_to_index_[mesh_infos_[i].mesh->GetEntityId()] = i;
        }
    }

    scene_loaded_ = true;
    return true;
}

void ForwardSceneRenderer::UnloadScene() {
    for (auto& ds : material_ds_) {
        if (ds != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(ds);
    }
    material_ds_.clear();
    for (auto& mi : mesh_infos_) {
        if (mi.mesh) mi.mesh->Destroy(device_);
        // 释放 content 侧注册的 RHIMeshAsset(SceneDataAdapter 注册,
        // 不释放则 rhi_mesh_assets free_list 在进程退出时 assert)
        if (mi.ownsMeshContentResource &&
            mi.meshEntityId != primal::id::invalid_id) {
            // meshEntityId 是 register_mesh_asset 返回的 geometry_hierarchies
            // 包装 id(内含 single_mesh_marker 假指针)— 走 destroy_resource
            content::destroy_resource(mi.meshEntityId, content::asset_type::mesh,
                                      content::GraphicsAPI::RHI);
        }
    }
    mesh_infos_.clear();
    material_textures_.clear();
    scene_loaded_ = false;
}

void ForwardSceneRenderer::UpdateAsyncTextures(u32 frame_index) {
    // TODO: Port async texture loading from TestParticleSponza when needed
    // For now, meshes use default white/normal textures
}

void ForwardSceneRenderer::SetIBLMaps(ResourceHandle irradiance, ResourceHandle prefiltered,
                                       ResourceHandle brdf_lut) {
    irradiance_map_ = irradiance;
    prefiltered_map_ = prefiltered;
    brdf_lut_ = brdf_lut;
    ibl_ready_ = (irradiance != handles::INVALID_RESOURCE &&
                  prefiltered != handles::INVALID_RESOURCE &&
                  brdf_lut != handles::INVALID_RESOURCE);
}

void ForwardSceneRenderer::SetAmbientLight(math::v3 color, float intensity) {
    ambient_light_ = {
        std::max(color.x, 0.0f),
        std::max(color.y, 0.0f),
        std::max(color.z, 0.0f),
        std::max(intensity, 0.0f),
    };
}

void ForwardSceneRenderer::ClearEnvironmentMap() {
    if (!device_) return;
    for (auto* texture : {&env_cube_, &irradiance_map_, &prefiltered_map_, &brdf_lut_}) {
        if (*texture != handles::INVALID_RESOURCE) {
            device_->DestroyTexture(*texture);
            *texture = handles::INVALID_RESOURCE;
        }
    }
    ibl_ready_ = false;
    env_map_path_.clear();
    skybox_enabled_ = false;
}

bool ForwardSceneRenderer::SetEnvironmentMap(const std::string& hdr_path) {
    if (hdr_path.empty() || !device_) return false;
    if (env_map_path_ == hdr_path && env_cube_ != handles::INVALID_RESOURCE) return true;

    if (!ibl_precomputer_) {
        ibl_precomputer_ = std::make_unique<rhi::IBLPrecomputer>(device_);
        if (!ibl_precomputer_->Initialize()) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] IBLPrecomputer init failed" << std::endl;
            ibl_precomputer_.reset();
            return false;
        }
    }

    // Decode equirect pixels — .hdr via stb (f32→f16), .texture (TEXR) via
    // container parse (ASTC blocks uploaded as-is, RGBA converted to f16).
    int w = 0, h = 0;
    DataFormat equirectFormat = DataFormat::RGBA16_Float;
    std::vector<u8> pixels;
    std::string ext = std::filesystem::path(hdr_path).extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);

    if (ext == ".texture") {
        u32 tw = 0, th = 0;
        if (!LoadTexrEquirect(hdr_path, tw, th, equirectFormat, pixels)) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to load TEXR: " << hdr_path << std::endl;
            return false;
        }
        w = (int)tw; h = (int)th;
        PRIMAL_LOG_STREAM(Info, "Render") << "[ForwardSceneRenderer] TEXR equirect: " << w << "x" << h << std::endl;
    } else {
        stbi_set_flip_vertically_on_load(1);
        int comp = 0;
        float* hdr = stbi_loadf(hdr_path.c_str(), &w, &h, &comp, 4);
        stbi_set_flip_vertically_on_load(0);
        if (!hdr || w <= 0 || h <= 0) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Failed to load HDR: " << hdr_path << std::endl;
            if (hdr) stbi_image_free(hdr);
            return false;
        }
        pixels.resize((size_t)w * h * 8);
        for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
            u16 v = f32_to_f16(hdr[i]);
            std::memcpy(pixels.data() + i * 2, &v, 2);
        }
        stbi_image_free(hdr);
    }

    // Upload equirect
    TextureDesc eqDesc{};
    eqDesc.size = {(u32)w, (u32)h, 1};
    eqDesc.format = equirectFormat;
    eqDesc.type = TextureType::Texture2D;
    eqDesc.mipLevels = 1;
    eqDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    ResourceHandle equirect = device_->CreateTexture(eqDesc);
    if (equirect == handles::INVALID_RESOURCE) {
        return false;
    }
    WriteTextureImmediate(device_, equirect, pixels.data(),
                          (u64)pixels.size(), (u32)w, (u32)h, 0);

    // Bake: equirect → cube → IBL triple
    ResourceHandle newCube = ibl_precomputer_->ComputeEnvironmentCube(equirect, 256);
    device_->DestroyTexture(equirect);
    if (newCube == handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Equirect→cube bake failed" << std::endl;
        return false;
    }
    ResourceHandle newIrr = ibl_precomputer_->ComputeIrradianceMap(newCube, 32);
    ResourceHandle newPre = ibl_precomputer_->ComputePrefilteredEnvironmentMap(newCube, 128);
    ResourceHandle newLut = ibl_precomputer_->ComputeBRDFIntegrationMap(512);
    if (newIrr == handles::INVALID_RESOURCE ||
        newPre == handles::INVALID_RESOURCE ||
        newLut == handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] IBL bake failed" << std::endl;
        for (auto h2 : {newCube, newIrr, newPre, newLut}) {
            if (h2 != handles::INVALID_RESOURCE) device_->DestroyTexture(h2);
        }
        return false;
    }

    // Swap in the new maps, release previous ones
    ResourceHandle oldCube = env_cube_;
    ResourceHandle oldIrr = irradiance_map_;
    ResourceHandle oldPre = prefiltered_map_;
    ResourceHandle oldLut = brdf_lut_;
    env_cube_ = newCube;
    SetIBLMaps(newIrr, newPre, newLut);
    env_map_path_ = hdr_path;
    for (auto h2 : {oldCube, oldIrr, oldPre, oldLut}) {
        if (h2 != handles::INVALID_RESOURCE) device_->DestroyTexture(h2);
    }
    PRIMAL_LOG_STREAM(Info, "Render") << "[ForwardSceneRenderer] Environment map set: " << hdr_path
              << " (" << w << "x" << h << ")" << std::endl;
    return true;
}


void ForwardSceneRenderer::SetLightDirection(math::v3 dir) { light_dir_ = dir; }
void ForwardSceneRenderer::SetLightColor(math::v4 color) { light_color_ = color; }
ParticlePass* ForwardSceneRenderer::GetParticlePass() { return &particle_pass_; }

SceneDataMeshInfo* ForwardSceneRenderer::GetMeshInfoMutable(u32 index) {
    if (index >= mesh_infos_.size()) return nullptr;
    return &mesh_infos_[index];
}

void ForwardSceneRenderer::GetMeshTextureHandles(u32 index, rhi::ResourceHandle& albedo,
                                                 rhi::ResourceHandle& normal, rhi::ResourceHandle& orm) const {
    albedo = normal = orm = rhi::handles::INVALID_RESOURCE;
    if (index >= mesh_infos_.size()) return;
    const u32 base = index * 3;
    if (base + 2 < material_textures_.size()) {
        albedo = material_textures_[base + 0];
        normal = material_textures_[base + 1];
        orm = material_textures_[base + 2];
    }
}

const SceneDataMeshInfo* ForwardSceneRenderer::GetMeshInfo(u32 index) const {
    if (index >= mesh_infos_.size()) return nullptr;
    return &mesh_infos_[index];
}

u32 ForwardSceneRenderer::GetMeshInfoCount() const {
    return static_cast<u32>(mesh_infos_.size());
}

void ForwardSceneRenderer::SetRenderScene(const RenderScene* scene) {
    render_scene_ = scene;
}

void ForwardSceneRenderer::SetGeometryEntities(const std::vector<id::id_type>& entity_ids) {
    geometry_entity_ids_ = entity_ids;
}

void ForwardSceneRenderer::ClearGeometryEntities() {
    geometry_entity_ids_.clear();
}

// ============================================================================
// VT M2 mesh 路径
// ============================================================================
void ForwardSceneRenderer::EnableVirtualTextures(const std::string& vtexAssetsDir) {
    vt_mode_ = !vtexAssetsDir.empty();
    vt_assets_dir_ = vtexAssetsDir;
    if (!vt_mode_ || !device_) return;
    auto& sys = graphics::vt::virtual_texture_system::instance();
    if (!sys.is_initialized()) {
        graphics::vt::vt_config cfg;
        // 每像素 3 层请求:3 × 宽 × 高(GBufferVT.frag 的 feedback 索引约定)
        cfg.feedback_slots = 3u * render_width_ * render_height_;
        sys.initialize(device_, cfg);
    }
    // 每材质参数块(5 × vec4 = 80B),上限 256 材质
    if (vt_material_params_buf_ == handles::INVALID_RESOURCE) {
        BufferDesc d{};
        d.size = 80ull * 256;
        d.type = BufferType::Constant;
        d.memoryUsage = GPUMemoryUsage::Dynamic;
        d.name = "FSR_VTMaterialParams";
        vt_material_params_buf_ = device_->CreateBuffer(d);
    }
}

void ForwardSceneRenderer::AttachVTFeedback(RHICommandBuffer* cb, u32 frame_index,
                                            QueueSubmitInfo& si) {
    if (!vt_mode_ || !cb) return;
    auto& sys = graphics::vt::virtual_texture_system::instance();
    if (!sys.is_initialized()) return;
    // VT 材质 set 的 binding 6 切到本帧 feedback 槽(每帧重绑)
    ResourceHandle fb = sys.feedback_buffer(frame_index);
    for (auto& ds : vt_material_ds_) {
        if (ds == handles::INVALID_DESCRIPTOR_SET) continue;
        DescriptorBufferInfo fInfo{fb, 0, 0};
        WriteDescriptorSet w{};
        w.dstSet = ds; w.dstBinding = 6; w.dstArrayElement = 0;
        w.descriptorCount = 1; w.descriptorType = DescriptorType::StorageBuffer;
        w.bufferInfo = &fInfo;
        device_->UpdateDescriptorSets(1, &w);
    }
    sys.record_feedback_copy(cb, frame_index, si);
}

bool ForwardSceneRenderer::TryCreateVTMaterial(const SceneDataMeshInfo& meshInfo, u32 meshIdx,
                                               const std::string& assetBaseDir) {
    if (!vt_mode_ || gbuffer_vt_pipeline_ == handles::INVALID_PIPELINE ||
        material_set_vt_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        return false;
    }
    auto& sys = graphics::vt::virtual_texture_system::instance();
    if (!sys.is_initialized()) return false;

    // 三槽 .vtex 候选:纹理路径同 stem 换扩展名,vtex 目录优先、原目录次之
    auto findVtex = [&](const std::string& texPath) -> std::string {
        if (texPath.empty()) return std::string();
        std::string stem = texPath;
        auto dot = stem.rfind('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        stem += ".vtex";
        for (const std::string& dir : {vt_assets_dir_, assetBaseDir}) {
            if (dir.empty()) continue;
            std::string p = dir.back() == '/' || dir.back() == '\\' ? dir + stem : dir + "/" + stem;
            std::ifstream f(p);
            if (f.good()) return p;
        }
        return std::string();
    };
    std::string paths[3] = {
        findVtex(meshInfo.diffuseTexturePath),
        findVtex(meshInfo.normalTexturePath),
        findVtex(meshInfo.ormTexturePath.empty()
                     ? (meshInfo.roughnessTexturePath.empty() ? meshInfo.metallicTexturePath
                                                              : meshInfo.roughnessTexturePath)
                     : meshInfo.ormTexturePath)};
    // 严格模式:albedo 必需;normal/orm 缺失时复用 albedo 的 VT(占位)
    if (paths[0].empty()) return false;

    auto registerVtex = [&](const std::string& path) -> u32 {
        auto it = vt_registered_.find(path);
        if (it != vt_registered_.end()) return it->second;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return ~0u;
        auto sz = f.tellg();
        f.seekg(0);
        std::vector<u8> blob(size_t(sz), 0u);
        f.read(reinterpret_cast<char*>(blob.data()), sz);
        u32 id = sys.register_texture(blob.data(), blob.size());
        if (id != ~0u) vt_registered_[path] = id;
        return id;
    };
    u32 ids[3];
    ids[0] = registerVtex(paths[0]);
    if (ids[0] == ~0u) return false;
    ids[1] = paths[1].empty() ? ids[0] : (registerVtex(paths[1]) == ~0u ? ids[0] : registerVtex(paths[1]));
    ids[2] = paths[2].empty() ? ids[0] : (registerVtex(paths[2]) == ~0u ? ids[0] : registerVtex(paths[2]));
    // 注:registerVtex 幂等(缓存),两次调用不会重复注册

    // 材质 set(7 绑定;feedback 绑定由 AttachVTFeedback 每帧切换)
    DescriptorSetDesc desc{};
    desc.layout = material_set_vt_layout_;
    DescriptorSetHandle ds = device_->CreateDescriptorSet(desc);
    if (ds == handles::INVALID_DESCRIPTOR_SET) return false;

    DescriptorImageInfo ptInfo{};
    ptInfo.imageView = sys.page_table_texture();
    ptInfo.imageLayout = ResourceState::ShaderResource;
    DescriptorImageInfo atlasInfo[3]{};
    for (int i = 0; i < 3; ++i) {
        atlasInfo[i].imageView = sys.atlas_texture(0);
        atlasInfo[i].imageLayout = ResourceState::ShaderResource;
    }
    DescriptorImageInfo sampInfo{};
    sampInfo.sampler = static_cast<ResourceHandle>(default_sampler_);
    DescriptorBufferInfo pInfo{vt_material_params_buf_, u64(meshIdx) * 80, 80};
    DescriptorBufferInfo fInfo{sys.feedback_buffer(0), 0, 0};

    WriteDescriptorSet w[7]{};
    for (u32 i = 0; i < 7; ++i) {
        w[i].dstSet = ds; w[i].dstBinding = i; w[i].dstArrayElement = 0;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = DescriptorType::SampledImage;   w[0].imageInfo = &ptInfo;
    w[1].descriptorType = DescriptorType::SampledImage;   w[1].imageInfo = &atlasInfo[0];
    w[2].descriptorType = DescriptorType::SampledImage;   w[2].imageInfo = &atlasInfo[1];
    w[3].descriptorType = DescriptorType::SampledImage;   w[3].imageInfo = &atlasInfo[2];
    w[4].descriptorType = DescriptorType::Sampler;        w[4].imageInfo = &sampInfo;
    w[5].descriptorType = DescriptorType::UniformBuffer;  w[5].bufferInfo = &pInfo;
    w[6].descriptorType = DescriptorType::StorageBuffer;  w[6].bufferInfo = &fInfo;
    device_->UpdateDescriptorSets(7, w);

    // 参数块(5 × vec4;与 GBufferVT.frag/VTMaterialParams 一致)
    struct P5 { float p[5][4]; } p{};
    for (int i = 0; i < 3; ++i) {
        p.p[i][0] = float(sys.vt_pt_origin_x(ids[i]));
        p.p[i][1] = float(sys.vt_pt_origin_y(ids[i]));
        p.p[i][2] = float(sys.vt_pages_top(ids[i]));
        p.p[i][3] = float(sys.vt_avg_color(ids[i]));
    }
    p.p[3][0] = 128.0f;
    p.p[3][1] = 1.0f / 2048.0f;
    p.p[3][2] = 1.0f; p.p[3][3] = 0.0f;
    p.p[4][0] = float(ids[0]);
    p.p[4][1] = float(ids[1]);
    p.p[4][2] = float(ids[2]);
    p.p[4][3] = 1024.0f;   // feedbackWidth(AttachVTFeedback 槽容量内)
    if (!device_->UpdateBufferData(vt_material_params_buf_, &p, sizeof(p), u64(meshIdx) * 80)) {
        device_->DestroyDescriptorSet(ds);
        return false;
    }

    if (meshIdx >= vt_material_ds_.size()) {
        vt_material_ds_.resize(meshIdx + 1, handles::INVALID_DESCRIPTOR_SET);
        material_is_vt_.resize(meshIdx + 1, false);
    }
    vt_material_ds_[meshIdx] = ds;
    material_is_vt_[meshIdx] = true;
    return true;
}

rhi::PipelineHandle ForwardSceneRenderer::GetTechniquePipeline(ShaderTechnique technique) const {
    switch (technique) {
        case ShaderTechnique::AlphaClip:   return alphaclip_pipeline_;
        case ShaderTechnique::Foliage:     return foliage_pipeline_;
        case ShaderTechnique::Water:       return water_pipeline_;
        case ShaderTechnique::Transparent: return transparent_pipeline_;
        case ShaderTechnique::Unlit:       return unlit_pipeline_;
        default:                           return gbuffer_pipeline_;
    }
}

rhi::PipelineHandle ForwardSceneRenderer::GetForwardTechniquePipeline(ShaderTechnique technique) const {
    switch (technique) {
        case ShaderTechnique::Water:       return forward_water_pipeline_;
        case ShaderTechnique::Transparent: return forward_transparent_pipeline_;
        default:                           return forward_water_pipeline_;
    }
}

ForwardSceneRenderer::StaticEntityResult
ForwardSceneRenderer::CreateStaticEntities() {
    StaticEntityResult result;
    result.entity_ids.reserve(mesh_infos_.size());
    result.mesh_slot_indices.reserve(mesh_infos_.size());

    for (u32 i = 0; i < mesh_infos_.size(); i++) {
        if (!mesh_infos_[i].mesh) continue;

        transform::init_info tf{};
        tf.position[0] = 0.f; tf.position[1] = 0.f; tf.position[2] = 0.f;
        tf.rotation[0] = 0.f; tf.rotation[1] = 0.f;
        tf.rotation[2] = 0.f; tf.rotation[3] = 1.f;
        tf.scale[0] = 1.f; tf.scale[1] = 1.f; tf.scale[2] = 1.f;

        game_entity::entity_info entity_info{};
        entity_info.transform = &tf;

        game_entity::entity entity = game_entity::create(entity_info);
        result.entity_ids.push_back(entity.get_id());
        result.mesh_slot_indices.push_back(i);
    }

    PRIMAL_LOG_STREAM(Info, "Render") << "[ForwardSceneRenderer] Created " << result.entity_ids.size()
              << " static ECS entities" << std::endl;
    return result;
}

void ForwardSceneRenderer::DestroyStaticEntities() {
    // Entity cleanup handled by caller via StandardRenderPipeline
}

u32 ForwardSceneRenderer::RegisterMeshResource(id::id_type geometry_content_id,
                                                id::id_type albedo_texture_id,
                                                id::id_type normal_texture_id,
                                                id::id_type orm_texture_id,
                                                id::id_type entity_id) {
    // Create RenderMesh from content system asset. Caller may pass a
    // distinct entity_id (StandardRenderPipeline::RegisterMeshEntity does
    // this) so multiple ECS entities can share the same geometry asset
    // without colliding in mesh_id_to_index_ (keyed by mesh.GetEntityId).
    const id::id_type mesh_entity_id = (entity_id != id::invalid_id) ? entity_id : geometry_content_id;
    RenderMesh* mesh = RenderMesh::CreateFromAsset(device_, geometry_content_id);
    if (!mesh) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] RegisterMeshResource: CreateFromAsset failed for id "
                  << geometry_content_id << std::endl;
        return (u32)-1;
    }
    if (mesh_entity_id != geometry_content_id) {
        mesh->Unregister();
        mesh->SetEntityId(mesh_entity_id);
        mesh->Register();
    }

    // Create SceneDataMeshInfo
    SceneDataMeshInfo info{};
    info.mesh = mesh;
    // meshEntityId is a content-system geometry ID, not the ECS entity ID.
    // Editor-registered scene entities borrow this asset from the content
    // system; the asset system releases it after the pipeline is gone.
    info.meshEntityId = geometry_content_id;
    info.ownsMeshContentResource = false;
    info.name = "Imported_" + std::to_string(mesh_entity_id);

    // Get texture handles from content system
    ResourceHandle albedo = white_texture_;
    ResourceHandle normal = flat_normal_texture_;
    ResourceHandle orm = white_texture_;

    if (albedo_texture_id != id::invalid_id) {
        auto h = content::get_rhi_texture_handle(albedo_texture_id);
        if (h != handles::INVALID_RESOURCE) albedo = h;
    }
    if (normal_texture_id != id::invalid_id) {
        auto h = content::get_rhi_texture_handle(normal_texture_id);
        if (h != handles::INVALID_RESOURCE) normal = h;
    }
    if (orm_texture_id != id::invalid_id) {
        auto h = content::get_rhi_texture_handle(orm_texture_id);
        if (h != handles::INVALID_RESOURCE) orm = h;
    }

    // Create material descriptor set
    DescriptorSetDesc desc{};
    desc.layout = material_set_layout_;
    auto matSet = device_->CreateDescriptorSet(desc);

    DescData params[] = {
        {0, DescriptorType::SampledImage, albedo},
        {1, DescriptorType::SampledImage, normal},
        {2, DescriptorType::SampledImage, orm},
        {3, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
    };
    UpdateDesc(device_, matSet, params, 4);

    // Store
    material_textures_.push_back(albedo);
    material_textures_.push_back(normal);
    material_textures_.push_back(orm);
    material_ds_.push_back(matSet);

    u32 slot = (u32)mesh_infos_.size();
    mesh_infos_.push_back(info);
    mesh_id_to_index_[mesh_entity_id] = slot;

    scene_loaded_ = true;
    return slot;
}

void ForwardSceneRenderer::UnregisterMeshResource(u32 slot_index) {
    if (slot_index >= mesh_infos_.size()) return;

    auto* mesh = mesh_infos_[slot_index].mesh;
    if (mesh) {
        mesh_id_to_index_.erase(mesh->GetEntityId());
        mesh->Destroy(device_);
        delete mesh;
        mesh_infos_[slot_index].mesh = nullptr;
    }

    if (slot_index < material_ds_.size() && material_ds_[slot_index] != handles::INVALID_DESCRIPTOR_SET) {
        device_->DestroyDescriptorSet(material_ds_[slot_index]);
        material_ds_[slot_index] = handles::INVALID_DESCRIPTOR_SET;
    }
}

// ============================================================================
// Material-graph compiled variants (editor world-material path)
// ============================================================================

u64 ForwardSceneRenderer::SetEntityGraphMaterial(id::id_type entity_id, const char* utf8_json) {
    if (!utf8_json || !*utf8_json || !device_ ||
        material_graph_vs_ == handles::INVALID_SHADER ||
        graph_material_layout_ == handles::INVALID_PIPELINE_LAYOUT) {
        return kInvalidGraphVariant;
    }
    const u64 hash = std::hash<std::string>{}(std::string(utf8_json));

    // Same content as the entity's current binding — nothing to do.
    auto existingBinding = entity_graph_variant_.find(entity_id);
    if (existingBinding != entity_graph_variant_.end() && existingBinding->second == hash &&
        graph_variants_.count(hash)) {
        return hash;
    }
    if (existingBinding != entity_graph_variant_.end()) {
        ReleaseGraphVariant(existingBinding->second);
        existingBinding->second = hash;
    } else {
        entity_graph_variant_.emplace(entity_id, hash);
    }

    auto vit = graph_variants_.find(hash);
    if (vit != graph_variants_.end()) {
        ++vit->second.refcount;
        return hash;
    }

    GraphVariant variant;

    // Translate → emit (world/GBuffer mode). texture_slots gives slot i →
    // asset_path exactly as the emitted texN arguments expect.
    material_graph::MaterialGraph graph;
    if (!material_graph::MaterialGraphSerializer::DeserializeIntoGraph(std::string(utf8_json), graph)) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] GraphMaterial: JSON deserialize failed for entity "
                  << entity_id << std::endl;
        entity_graph_variant_.erase(entity_id);
        return kInvalidGraphVariant;
    }
    shader_ir::MaterialGraphToIR translator;
    auto ir = translator.Translate(graph);
    shader_ir::MetalEmitter emitter;
    auto emitted = emitter.Emit(ir.function, /*preview_mode=*/false);
    if (!ir.success || !emitted.success || ir.texture_slots.size() > kMaxGraphTextures) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] GraphMaterial: compile failed for entity "
                  << entity_id << " (textures=" << ir.texture_slots.size()
                  << ", translated=" << (ir.success ? 1 : 0)
                  << ", emitted=" << (emitted.success ? 1 : 0) << ")" << std::endl;
        entity_graph_variant_.erase(entity_id);
        return kInvalidGraphVariant;
    }

    variant.pixelShader = device_->CreateShader(emitted.source.data(), emitted.source.size(),
                                                ShaderStage::Pixel, "fragmentMain");
    if (variant.pixelShader == handles::INVALID_SHADER) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] GraphMaterial: pixel shader creation failed for entity "
                  << entity_id << std::endl;
        entity_graph_variant_.erase(entity_id);
        return kInvalidGraphVariant;
    }
    // Diagnostics: PRIMAL_DUMP_GRAPH_MSL=<path> writes the emitted fragment
    // source next to the registration log.
    if (const char* dump = std::getenv("PRIMAL_DUMP_GRAPH_MSL")) {
        std::ofstream df(dump, std::ios::binary);
        if (df) { df.write(emitted.source.data(), (std::streamsize)emitted.source.size()); }
        PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] GraphMaterial MSL dumped to " << dump << std::endl;
    }

    // Import the graph's textures on THIS pipeline's device (same rationale
    // as PipelineImportCookedTexture / MaterialPreviewRenderer: editor-owned
    // devices are not registered in the legacy global device manager).
    // Slots that fail to import fall back to the white texture instead of
    // dropping the whole compiled path.
    variant.texture_ids.resize(ir.texture_slots.size(), id::invalid_id);
    utl::vector<ResourceHandle> slot_handles(kMaxGraphTextures, white_texture_);
    for (u32 i = 0; i < ir.texture_slots.size(); ++i) {
        const std::string& path = ir.texture_slots[i];
        if (path.empty()) continue;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) continue;
        const auto size = file.tellg();
        if (size <= 0) continue;
        file.seekg(0, std::ios::beg);
        std::vector<u8> bytes((size_t)size);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) continue;
        const auto tex_id = content::create_resource(bytes.data(), content::asset_type::texture,
                                                     content::GraphicsAPI::RHI, device_);
        if (tex_id == id::invalid_id || content::get_rhi_texture_handle(tex_id) == handles::INVALID_RESOURCE) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] GraphMaterial: texture import failed '" << path << "'" << std::endl;
            continue;
        }
        variant.texture_ids[i] = tex_id;
        slot_handles[i] = content::get_rhi_texture_handle(tex_id);
    }

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = graph_material_set_layout_;
    variant.materialSet = device_->CreateDescriptorSet(dsDesc);
    DescData params[kMaxGraphTextures + 1];
    for (u32 i = 0; i < kMaxGraphTextures; ++i) {
        params[i] = {i, DescriptorType::SampledImage, slot_handles[i]};
    }
    params[kMaxGraphTextures] = {kMaxGraphTextures, DescriptorType::Sampler,
                                 static_cast<ResourceHandle>(default_sampler_)};
    UpdateDesc(device_, variant.materialSet, params, kMaxGraphTextures + 1);

    GraphicsPipelineDesc desc{};
    desc.layout = graph_material_layout_;
    desc.vertexShader = material_graph_vs_;
    desc.pixelShader = variant.pixelShader;
    desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    desc.renderTargetFormats[1] = DataFormat::RGBA16_Float;
    desc.renderTargetFormats[2] = DataFormat::BGRA8_UNorm;
    desc.renderTargetFormats[3] = DataFormat::RG16_Float;
    desc.renderTargetCount = 4;
    desc.depthStencilFormat = DataFormat::D32_Float;
    desc.enableDepthTest = true;
    desc.enableDepthWrite = true;
    desc.depthFunc = ComparisonFunc::Less;
    desc.cullMode = CullMode::None;
    ConfigureGeometryVertexLayout(desc);
    variant.pipeline = device_->CreateGraphicsPipeline(desc);

    variant.refcount = 1;
    graph_variants_.emplace(hash, std::move(variant));
    PRIMAL_LOG_STREAM(Info, "Render") << "[ForwardSceneRenderer] GraphMaterial registered for entity " << entity_id
              << " (hash=" << std::hex << hash << std::dec
              << ", textures=" << ir.texture_slots.size() << ")" << std::endl;
    return hash;
}

void ForwardSceneRenderer::ReleaseGraphVariant(u64 variant_hash) {
    auto vit = graph_variants_.find(variant_hash);
    if (vit == graph_variants_.end()) return;
    if (--vit->second.refcount > 0) return;

    GraphVariant& v = vit->second;
    if (v.materialSet != handles::INVALID_DESCRIPTOR_SET) { device_->DestroyDescriptorSet(v.materialSet); }
    if (v.pipeline != handles::INVALID_PIPELINE) { device_->DestroyPipeline(v.pipeline); }
    if (v.pixelShader != handles::INVALID_SHADER) { device_->DestroyShader(v.pixelShader); }
    for (auto tex_id : v.texture_ids) {
        if (tex_id != id::invalid_id) content::destroy_resource(tex_id, content::asset_type::texture);
    }
    graph_variants_.erase(vit);
}

void ForwardSceneRenderer::ClearEntityGraphMaterial(id::id_type entity_id) {
    auto it = entity_graph_variant_.find(entity_id);
    if (it == entity_graph_variant_.end()) return;
    ReleaseGraphVariant(it->second);
    entity_graph_variant_.erase(it);
}

void ForwardSceneRenderer::PurgeGraphMaterials() {
    for (auto& [_, v] : graph_variants_) {
        if (v.materialSet != handles::INVALID_DESCRIPTOR_SET && device_) { device_->DestroyDescriptorSet(v.materialSet); }
        if (v.pipeline != handles::INVALID_PIPELINE && device_) { device_->DestroyPipeline(v.pipeline); }
        if (v.pixelShader != handles::INVALID_SHADER && device_) { device_->DestroyShader(v.pixelShader); }
        for (auto tex_id : v.texture_ids) {
            if (tex_id != id::invalid_id) content::destroy_resource(tex_id, content::asset_type::texture);
        }
    }
    graph_variants_.clear();
    entity_graph_variant_.clear();
}

void ForwardSceneRenderer::RenderDynamicInstances(RHICommandBuffer* cmd,
                                                    const math::m4x4& vp_matrix,
                                                    u32 frame_index,
                                                    bool shadow_pass,
                                                    bool forward_pass) {
    if (!render_scene_ || mesh_id_to_index_.empty()) return;

    // Frustum cull only for shadow passes — GBuffer uses all proxies (GPU clips invisible triangles)
    utl::vector<const RenderProxy*> visible;
    if (shadow_pass) {
        rhi::Frustum frustum;
        frustum.FromMatrix(vp_matrix);
        visible = render_scene_->Cull(frustum);
        if (visible.empty() && !render_scene_->GetProxies().empty()) {
            static bool warned = false;
            if (!warned) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[ForwardSceneRenderer] Shadow cull: 0/" << render_scene_->GetProxies().size()
                          << " proxies visible. AABB issue?" << std::endl;
                auto& proxies = render_scene_->GetProxies();
                if (!proxies.empty()) {
                    auto& p = proxies[0];
                    PRIMAL_LOG_STREAM(Error, "Render") << "  Proxy 0 AABB: min=(" << p.worldAABB.min.x << "," << p.worldAABB.min.y << "," << p.worldAABB.min.z
                              << ") max=(" << p.worldAABB.max.x << "," << p.worldAABB.max.y << "," << p.worldAABB.max.z
                              << ") valid=" << p.worldAABB.IsValid() << std::endl;
                }
                warned = true;
            }
        }
    } else {
        for (auto& p : render_scene_->GetProxies()) {
            visible.push_back(&p);
        }
    }
    if (visible.empty()) return;

    // Debug: first frame shadow pass stats
    static bool shadowDebug = true;
    if (shadowDebug && shadow_pass) {
        PRIMAL_LOG_STREAM(Info, "Render") << "[ShadowPass] visible=" << visible.size()
                  << " total_proxies=" << (render_scene_ ? render_scene_->GetProxies().size() : 0)
                  << " mesh_map=" << mesh_id_to_index_.size() << std::endl;
        shadowDebug = false;
    }

    // Group visible proxies by technique then by mesh_infos_ index
    u32 mesh_count = static_cast<u32>(mesh_infos_.size());
    std::array<std::unordered_map<u32, std::vector<graphics::ForwardInstanceData>>, TechniqueCount> technique_groups;
    // Compiled material-graph proxies (editor world-material path). Only the
    // opaque GBuffer pass consumes them; shadow and forward-blended passes
    // fall through to the fixed technique path.
    std::unordered_map<u64, std::unordered_map<u32, std::vector<graphics::ForwardInstanceData>>> graph_groups;
    for (auto* proxy : visible) {
        auto it = mesh_id_to_index_.find(proxy->meshId);
        if (it == mesh_id_to_index_.end()) continue;
        u32 meshIdx = it->second;
        if (meshIdx >= mesh_count || !mesh_infos_[meshIdx].mesh) continue;

        if (!shadow_pass && !forward_pass &&
            proxy->graphVariant != kInvalidGraphVariant &&
            proxy->graphVariant != 0 &&
            static_cast<u32>(proxy->technique) == 0 /* opaque */ &&
            graph_variants_.count(proxy->graphVariant)) {
            graphics::ForwardInstanceData inst{};
            inst.transform = proxy->transform;
            memcpy(inst.base_color, proxy->base_color, sizeof(f32) * 4);
            inst.roughness    = proxy->roughness;
            inst.metallic     = proxy->metallic;
            inst.alpha_cutoff = proxy->alpha_cutoff;
            inst.transmission = proxy->transmission;
            graph_groups[proxy->graphVariant][meshIdx].push_back(inst);
            continue;
        }

        u32 t = static_cast<u32>(proxy->technique);
        if (t >= TechniqueCount) t = 0;
        graphics::ForwardInstanceData inst{};
        inst.transform = proxy->transform;
        memcpy(inst.base_color, proxy->base_color, sizeof(f32) * 4);
        inst.roughness    = proxy->roughness;
        inst.metallic     = proxy->metallic;
        inst.alpha_cutoff = proxy->alpha_cutoff;
        inst.transmission = proxy->transmission;
        technique_groups[t][meshIdx].push_back(inst);
    }

    // Check if any groups exist
    bool has_any = !graph_groups.empty();
    if (!has_any) {
        for (u32 t = 0; t < TechniqueCount && !has_any; ++t) {
            if (!technique_groups[t].empty()) { has_any = true; break; }
        }
    }
    if (!has_any) return;

    // Compute total instance count and ensure buffer capacity
    u32 total = 0;
    for (u32 t = 0; t < TechniqueCount; ++t)
        for (auto& [_, transforms] : technique_groups[t])
            total += (u32)transforms.size();
    for (auto& [_, meshMap] : graph_groups)
        for (auto& [_, transforms] : meshMap)
            total += (u32)transforms.size();
    u32 required_size = total * sizeof(graphics::ForwardInstanceData);
    if (pcg_instance_buffer_capacity_ < required_size) {
        if (pcg_instance_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(pcg_instance_buffer_);
        }
        BufferDesc desc{};
        desc.size = required_size;
        desc.type = BufferType::Vertex;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        pcg_instance_buffer_ = device_->CreateBuffer(desc);
        pcg_instance_buffer_capacity_ = required_size;
    }

    // Upload all grouped transforms
    void* buf = device_->MapBuffer(pcg_instance_buffer_, 0, required_size);
    if (!buf) return;

    // Compute group offsets per technique
    struct GroupRange { u32 offset; u32 count; };
    std::array<std::unordered_map<u32, GroupRange>, TechniqueCount> all_ranges;
    std::unordered_map<u64, std::unordered_map<u32, GroupRange>> graph_ranges;
    u32 off = 0;
    for (u32 t = 0; t < TechniqueCount; ++t) {
        for (auto& [meshIdx, instances] : technique_groups[t]) {
            all_ranges[t][meshIdx] = {off, (u32)instances.size()};
            for (u32 j = 0; j < instances.size(); j++) {
                ((graphics::ForwardInstanceData*)buf)[off + j] = instances[j];
            }
            off += (u32)instances.size();
        }
    }
    for (auto& [hash, meshMap] : graph_groups) {
        for (auto& [meshIdx, instances] : meshMap) {
            graph_ranges[hash][meshIdx] = {off, (u32)instances.size()};
            for (u32 j = 0; j < instances.size(); j++) {
                ((graphics::ForwardInstanceData*)buf)[off + j] = instances[j];
            }
            off += (u32)instances.size();
        }
    }
    device_->UnmapBuffer(pcg_instance_buffer_);

    // Select layout based on pass type
    auto layout = shadow_pass ? shadow_layout_ : gbuffer_layout_;

    // Draw each technique bucket in render order
    bool vtPipelineBound_ = false;   // VT 管线覆盖状态(桶内逐 mesh 切换)
    for (u32 t = 0; t < TechniqueCount; ++t) {
        if (all_ranges[t].empty()) continue;

        // Editor forward path: render everything (Opaque, AlphaClip,
        // Foliage, Water, Transparent) so the editor preview matches what
        // the production pipeline would composite.
        if (!editor_mode_) {
            if (forward_pass && t != 3 && t != 4) continue;
            if (!shadow_pass && !forward_pass && (t == 3 || t == 4)) continue;
        }

        // Bind technique-specific pipeline for GBuffer or forward pass
        if (!shadow_pass) {
            if (forward_pass) {
                cmd->BindGraphicsPipeline(GetForwardTechniquePipeline(static_cast<ShaderTechnique>(t)));
            } else {
                cmd->BindGraphicsPipeline(GetTechniquePipeline(static_cast<ShaderTechnique>(t)));
            }
        }

        for (auto& [meshIdx, range] : all_ranges[t]) {
            // T4.6.5 part 15.2: instance buffer bound via SSBO descriptor at binding 2
            // on Vulkan; Metal still uses BindVertexBuffers(3, 1, ...) at vertex stage.
            u64 inst_offset = range.offset * sizeof(graphics::ForwardInstanceData);
#ifdef __EMSCRIPTEN__
            // WASM uses standard vertex attributes (slot 0) + instance-rate
            // attributes (slot 1). Metal keeps the legacy slot-20/3 vertex-pull.
            cmd->BindVertexBuffers(1, 1, &pcg_instance_buffer_, &inst_offset);
#else
            if (device_->GetPlatform() != RHIPlatform::Vulkan) {
                cmd->BindVertexBuffers(3, 1, &pcg_instance_buffer_, &inst_offset);
            }
#endif
            auto matSet = (meshIdx < material_ds_.size()) ? material_ds_[meshIdx] : material_ds_[0];
            // VT M2:材质为 VT 且非 shadow/forward 桶 → 覆盖管线/set1(布局随管线)
            const bool meshVT = !shadow_pass && !forward_pass &&
                                meshIdx < material_is_vt_.size() && material_is_vt_[meshIdx] &&
                                gbuffer_vt_pipeline_ != handles::INVALID_PIPELINE &&
                                meshIdx < vt_material_ds_.size() &&
                                vt_material_ds_[meshIdx] != handles::INVALID_DESCRIPTOR_SET;
            if (meshVT) {
                cmd->BindGraphicsPipeline(gbuffer_vt_pipeline_);
                matSet = vt_material_ds_[meshIdx];
            } else if (vtPipelineBound_) {
                // 前一 mesh 是 VT,恢复桶管线
                cmd->BindGraphicsPipeline(GetTechniquePipeline(static_cast<ShaderTechnique>(t)));
            }
            vtPipelineBound_ = meshVT;
            const DescriptorSetHandle matSets[] = {matSet};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics,
                                    meshVT ? gbuffer_vt_layout_ : layout, 1, 1, matSets, 0, nullptr);
            PCGPushConsts pc{};
            pc.transform = MatrixIdentity();
            pc.use_instances = 1;
            cmd->PushConstants(layout, ShaderStage::Vertex, 0, sizeof(PCGPushConsts), &pc);
#ifdef __EMSCRIPTEN__
            mesh_infos_[meshIdx].mesh->Draw(cmd, range.count, 0, 0);
#else
            if (device_->GetPlatform() == RHIPlatform::Vulkan) {
                // Vulkan:管线顶点输入在 binding 0(单 interleaved VB);
                // 实例经 set0 binding2 SSBO,gl_InstanceIndex 需绝对偏移
                mesh_infos_[meshIdx].mesh->Draw(cmd, range.count, range.offset, 0);
            } else {
                mesh_infos_[meshIdx].mesh->Draw(cmd, range.count, 0, 20);
            }
#endif
        }
    }

    // Compiled material-graph entities (editor world-material path). Same
    // GBuffer attachments/push constants as the fixed path; only the
    // pipeline and the texture set differ (per-variant, sampler at binding 8).
    static const bool kGraphDrawDisabled = std::getenv("PRIMAL_DISABLE_GRAPH_DRAW") != nullptr;
    if (!shadow_pass && !forward_pass && !kGraphDrawDisabled && !graph_ranges.empty() &&
        material_graph_vs_ != handles::INVALID_SHADER) {
        const DescriptorSetHandle globalSets[] = {global_ds_[frame_index]};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, graph_material_layout_, 0, 1, globalSets, 0, nullptr);

        for (auto& [hash, meshMap] : graph_ranges) {
            auto vit = graph_variants_.find(hash);
            if (vit == graph_variants_.end()) continue;
            GraphVariant& variant = vit->second;

            cmd->BindGraphicsPipeline(variant.pipeline);
            const DescriptorSetHandle matSets[] = {variant.materialSet};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, graph_material_layout_, 1, 1, matSets, 0, nullptr);
            PCGPushConsts pc{};
            pc.transform = MatrixIdentity();
            pc.use_instances = 1;
            cmd->PushConstants(graph_material_layout_, ShaderStage::Vertex, 0, sizeof(PCGPushConsts), &pc);

            for (auto& [meshIdx, range] : meshMap) {
                u64 inst_offset = range.offset * sizeof(graphics::ForwardInstanceData);
                cmd->BindVertexBuffers(3, 1, &pcg_instance_buffer_, &inst_offset);
                mesh_infos_[meshIdx].mesh->Draw(cmd, range.count, 0, 20);
            }
        }
    }
}

// ============================================================================
// Shadow VP Computation
// ============================================================================

math::m4x4 ForwardSceneRenderer::ComputeCascadeVP(math::v3 lightDir, math::v3 camera_pos,
                                                    float ortho_extent, u32 shadow_map_size, u32 cascade,
                                                    float near_plane, float far_plane) {
    math::v3 lightUp{0.0f, 1.0f, 0.0f};
    if (std::abs(lightDir.y) > 0.9f) lightUp = {1.0f, 0.0f, 0.0f};

    // Match TestParticleSponza convention: lightEye = -lightDir * distance, looking at origin
    float cascadeDistance = 200.0f;
    math::v3 origin{0, 0, 0};
    math::v3 lightEye = {
        -lightDir.x * cascadeDistance,
        -lightDir.y * cascadeDistance,
        -lightDir.z * cascadeDistance
    };
    math::m4x4 lightView = CreateLookAtMatrix(lightEye, origin, lightUp);
    math::m4x4 lightProj = CreateOrthographicMatrix(-ortho_extent, ortho_extent,
                                                     ortho_extent, -ortho_extent,
                                                     near_plane, far_plane);
    cached_shadow_vp_[cascade] = lightProj * lightView;
    return cached_shadow_vp_[cascade];
}

// ============================================================================
// Streaming Mesh Pass (Phase 9.3b Task 12)
// ============================================================================

void ForwardSceneRenderer::RenderStreamingMeshes(RHICommandBuffer* cmd, u32 frame_index) {
    if (!render_scene_) return;
#ifdef __EMSCRIPTEN__
    // StreamingMesh binds positions/elements/indices at slots 20/21/22 with
    // DrawIndirect — separate WASM port needed (slot-20 vertex pull won't work
    // on Dawn). WFC demo uses RegisterMeshEntity path, not StreamingMesh, so
    // skip on WASM until a separate port lands.
    return;
#endif
    if (streaming_pipeline_ == handles::INVALID_PIPELINE) return;
    if (streaming_material_ds_ == handles::INVALID_DESCRIPTOR_SET) return;

    // Triple-buffer slot: producer (GlobalSDFMeshNode::Execute) wrote slot
    // frame_index%MAX_FRAMES_IN_FLIGHT this frame; we read the matching slot.
    // Other slots' records are skipped — their GPU buffers are either being
    // read by an in-flight render cmd buffer or hold stale data.
    const u32 target_slot = frame_index % 3;

    // Early-out: any visible, non-tombstoned, valid streaming mesh for this slot?
    bool any_visible = false;
    render_scene_->ForEachStreamingMesh([&](const StreamingMeshRecord& sm) {
        if (sm.slot != target_slot) return;
        if (sm.visible && !sm.tombstoned && sm.mesh && sm.mesh->IsValid()) any_visible = true;
    });
    if (!any_visible) return;

    // Re-enter GBuffer render pass with Load op (preserve Pass 3's color+depth).
    RenderPassDesc rpDesc{};
    rpDesc.colorAttachments.resize(4);
    rpDesc.colorAttachments[0].texture = gbuffer_albedo_[frame_index];
    rpDesc.colorAttachments[0].loadOp = LoadAction::Load;
    rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
    rpDesc.colorAttachments[1].texture = gbuffer_normal_[frame_index];
    rpDesc.colorAttachments[1].loadOp = LoadAction::Load;
    rpDesc.colorAttachments[1].storeOp = StoreAction::Store;
    rpDesc.colorAttachments[2].texture = gbuffer_orm_[frame_index];
    rpDesc.colorAttachments[2].loadOp = LoadAction::Load;
    rpDesc.colorAttachments[2].storeOp = StoreAction::Store;
    rpDesc.colorAttachments[3].texture = gbuffer_velocity_[frame_index];
    rpDesc.colorAttachments[3].loadOp = LoadAction::Load;
    rpDesc.colorAttachments[3].storeOp = StoreAction::Store;
    rpDesc.depthAttachment.texture = gbuffer_depth_[frame_index];
    rpDesc.depthAttachment.loadOp = LoadAction::Load;
    rpDesc.depthAttachment.storeOp = StoreAction::Store;
    cmd->BeginRenderPass(rpDesc);

    cmd->BindGraphicsPipeline(streaming_pipeline_);
    cmd->SetViewport({{0, 0}, {(float)render_width_, (float)render_height_}, 0, 1});
    cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

    const DescriptorSetHandle globalSets[] = {global_ds_[frame_index]};
    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbuffer_layout_, 0, 1, globalSets, 0, nullptr);
    const DescriptorSetHandle matSets[] = {streaming_material_ds_};
    cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbuffer_layout_, 1, 1, matSets, 0, nullptr);

    // StreamingMesh positions are already in world space; identity transform.
    PCGPushConsts pc{};
    pc.transform = MatrixIdentity();
    pc.use_instances = 0;
    cmd->PushConstants(gbuffer_layout_, ShaderStage::Vertex, 0, sizeof(PCGPushConsts), &pc);

    render_scene_->ForEachStreamingMesh([&](const StreamingMeshRecord& sm) {
        if (sm.slot != target_slot) return;  // skip non-matching triple-buffer slots
        if (!sm.visible || sm.tombstoned) return;
        if (sm.mesh == nullptr || !sm.mesh->IsValid()) return;

        // Diagnostic: dump indirect_args + counters to find where vertexCount
        // corruption originates. counters[0]=vert_count, counters[1]=idx_count;
        // indirect_args.vertexCount should equal counters[1] (set by
        // write_indirect_args). If they diverge, write_indirect_args is racing
        // or never ran. If both equal 504978, emit_faces is over-producing.
        static u32 s_render_diag = 0;
        if (s_render_diag < 200) {
            auto* args = static_cast<u32*>(device_->MapBuffer(sm.mesh->indirect_args));
            auto* cnt  = static_cast<u32*>(device_->MapBuffer(sm.mesh->counters));
            if (args && cnt) {
                PRIMAL_LOG_STREAM(Info, "Render") << "[RenderStream] frame_idx=" << frame_index
                          << " slot=" << sm.slot
                          << " args=(vc=" << args[0]
                          << " ic=" << args[1]
                          << " vs=" << args[2]
                          << " bi=" << args[3] << ")"
                          << " cnt=(v=" << cnt[0]
                          << " i=" << cnt[1] << ")"
                          << std::endl;
            }
            if (args) device_->UnmapBuffer(sm.mesh->indirect_args);
            if (cnt)  device_->UnmapBuffer(sm.mesh->counters);
            ++s_render_diag;
        }

        // T4.6.5 part 15.4: Vulkan writes SoA buffers as SSBOs at bindings 3/4/5
        // of global_ds_ per-mesh; Metal keeps BindVertexBuffers(20, 3, ...) vertex
        // slot semantics. Per-mesh UpdateDesc churn is accepted for correctness.
        if (device_->GetPlatform() == RHIPlatform::Vulkan) {
            DescData soaParams[] = {
                {3, DescriptorType::StorageBuffer, sm.mesh->positions},
                {4, DescriptorType::StorageBuffer, sm.mesh->elements},
                {5, DescriptorType::StorageBuffer, sm.mesh->indices},
            };
            UpdateDesc(device_, global_ds_[frame_index], soaParams, 3);
            const DescriptorSetHandle globalSets[] = {global_ds_[frame_index]};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbuffer_layout_, 0, 1, globalSets, 0, nullptr);
        } else {
            // Bind positions (20), elements (21), indices (22). The shader does
            // manual indexed drawing — DrawIndirect issues idx_count invocations,
            // and indices[vid] maps each invocation to the actual vertex.
            ResourceHandle vb[3] = { sm.mesh->positions, sm.mesh->elements, sm.mesh->indices };
            u64 offsets[3] = { 0, 0, 0 };
            cmd->BindVertexBuffers(20, 3, vb, offsets);
        }
        cmd->DrawIndirect(sm.mesh->indirect_args, 0, 1);
    });

    cmd->EndRenderPass();
}

// ============================================================================
// Main Render Method
// ============================================================================

void ForwardSceneRenderer::Render(RHICommandBuffer* cmd,
                                    const math::m4x4& view_matrix,
                                    const math::m4x4& proj_matrix,
                                    const math::v3& camera_position,
                                    ResourceHandle backbuffer,
                                    u32 frame_index,
                                    DataFormat backbuffer_format) {
    if (!initialized_ || !scene_loaded_) return;

    u32 idx = frame_index % 3;

    // Lazily recreate the blit pipeline if the caller changed target format
    // (e.g., headless creates RGBA16F, editor's MTKView backbuffer is BGRA8).
    if (backbuffer_format != blit_target_format_) {
        blit_target_format_ = backbuffer_format;
        GraphicsPipelineDesc desc{};
        desc.layout = blit_layout_;
        desc.vertexShader = blit_vs_;
        desc.pixelShader = blit_ps_;
        desc.renderTargetFormats[0] = backbuffer_format;
        desc.renderTargetCount = 1;
        desc.depthStencilFormat = DataFormat::Unknown;
        desc.enableDepthTest = false;
        desc.enableDepthWrite = false;
        desc.cullMode = CullMode::None;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        blit_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }

    // Debug: first frame only
    static bool first_frame = true;
    if (first_frame) {
        size_t proxyCount = render_scene_ ? render_scene_->GetProxies().size() : 0;
        PRIMAL_LOG_STREAM(Info, "Render") << "[ForwardSceneRenderer::Render] scene_loaded_=" << scene_loaded_
                  << " proxies=" << proxyCount << " mesh_infos=" << mesh_infos_.size()
                  << " mesh_id_map=" << mesh_id_to_index_.size() << std::endl;
        first_frame = false;
    }

    // Update shadow VPs — consume lights from RenderScene if available
    // Default light position (fallback when no RenderScene or no directional lights)
    math::v3 lightPos{100.0f, 150.0f, 50.0f};
    math::v4 effectiveLightColor = light_color_;

    // === Phase 4.5: Pull ECS Light components into RenderScene ===
    // 在读 GetLights() 之前先同步，让 ECS Light 组件真正驱动渲染。
    // SyncLightsFromECS 内部会 ClearLights() + 重新填充，所以幂等可重入。
    //
    // const_cast 说明：render_scene_ 设计上是 const（renderer 其他路径只读），
    // 但 LightSyncSystem 是 per-frame 的状态准备阶段，需要写入 lights_。
    // RenderScene 的 mutator 用 mutable mutex_ 保护，逻辑上 const-correct，
    // 但 API 上没标 const。这里 const_cast 是最小侵入的接口适配方式，
    // 避免改 SetRenderScene 签名扩散到 StandardRenderPipeline。
    if (render_scene_) {
        scene_sync::SyncLightsFromECS(const_cast<RenderScene&>(*render_scene_));
    }

    // Override with RenderScene directional light if available
    if (render_scene_) {
        const auto& allLights = render_scene_->GetLights();
        for (const auto& rl : allLights) {
            if (rl.type == LightType::Directional) {
                // lightPos is opposite of direction (direction points from light to scene)
                math::v3 dir = rl.direction;
                float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
                if (len > 0.001f) {
                    // Use a fixed distance for the shadow eye position
                    float dist = 200.0f;
                    lightPos = { -dir.x * dist, -dir.y * dist, -dir.z * dist };
                }
                effectiveLightColor = { rl.color.x, rl.color.y, rl.color.z, rl.intensity };
                break; // Use first directional light only
            }
        }
    }

    math::v3 lightDir = Normalize(math::v3{-lightPos.x, -lightPos.y, -lightPos.z});
    ComputeCascadeVP(lightDir, camera_position, 30.0f, 2048, 0, 0.1f, 1000.0f);
    ComputeCascadeVP(lightDir, camera_position, 500.0f, 2048, 1, 0.1f, 5000.0f);

    // Update ViewData CB
    {
        ViewData vd{};
        vd.viewProjection = proj_matrix * view_matrix;
        vd.invViewProjection = Inverse(vd.viewProjection);
        vd.previousViewProjection = vd.viewProjection;
        auto* mapped = static_cast<ViewData*>(device_->MapBuffer(view_cb_[idx]));
        if (mapped) { *mapped = vd; device_->UnmapBuffer(view_cb_[idx]); }
    }
    // Update SceneData CB
    {
        SceneData sd{};
        sd.model = MatrixIdentity();
        sd.previousModel = MatrixIdentity();
        sd.lightPos = {lightPos.x, lightPos.y, lightPos.z, 0.0f};
        sd.lightColor = effectiveLightColor;
        static auto start_time = std::chrono::steady_clock::now();
        sd.time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time).count();
        sd.viewPos = {camera_position.x, camera_position.y, camera_position.z, 1.0f};
        sd.shadowMatrix0 = cached_shadow_vp_[0];
        sd.shadowMatrix1 = cached_shadow_vp_[1];
        sd.shadowParams = {0.0f, 50.0f, 0.0f, 0.0f};
        sd.ambientLight = ambient_light_;
        auto* mapped = static_cast<SceneData*>(device_->MapBuffer(scene_cb_[idx]));
        if (mapped) { *mapped = sd; device_->UnmapBuffer(scene_cb_[idx]); }
    }

    // Update global descriptor set
    {
        DescData params[] = {
            {0, DescriptorType::UniformBuffer, view_cb_[idx]},
            {1, DescriptorType::UniformBuffer, scene_cb_[idx]},
        };
        UpdateDesc(device_, global_ds_[idx], params, 2);
    }

    // T4.6.5 part 15.2: Vulkan wires pcg_instance_buffer_ as SSBO at binding 2 of
    // global_ds_. Metal path uses BindVertexBuffers(3, 1, ...) at vertex stage —
    // see RenderDynamicInstances (cpp:1621) — which has no equivalent on Vulkan.
    if (device_->GetPlatform() == RHIPlatform::Vulkan &&
        pcg_instance_buffer_ != handles::INVALID_RESOURCE) {
        DescData instParam = {2, DescriptorType::StorageBuffer, pcg_instance_buffer_};
        UpdateDesc(device_, global_ds_[idx], &instParam, 1);
    }
    // Fill the GPU light array with ALL scene lights (multi-light lighting).
    // fragmentLighting_v3 loops directionals (sun = [0], the only caster with
    // cascade shadows) + punctual lights. Count/attenuation mapping mirrors
    // ForwardRenderer::SetupLights so both paths agree on semantics.
    {
        auto* lb = static_cast<rhi::ForwardLightBuffer*>(device_->MapBuffer(light_cb_[idx]));
        if (lb) {
            std::memset(lb, 0, sizeof(rhi::ForwardLightBuffer));
            if (render_scene_) {
                const auto& allLights = render_scene_->GetLights();
                for (const auto& rl : allLights) {
                    if (rl.type == LightType::Directional) {
                        if (lb->directionalLightCount >= 4) continue;
                        auto& dl = lb->directionalLights[lb->directionalLightCount++];
                        dl.directionAndIntensity = { rl.direction.x, rl.direction.y, rl.direction.z, rl.intensity };
                        dl.colorAndShadow = { rl.color.x, rl.color.y, rl.color.z, 1.0f };
                    } else {
                        if (lb->punctualLightCount >= 128) continue;
                        auto& pl = lb->lights[lb->punctualLightCount++];
                        pl.position = rl.position;
                        pl.intensity = rl.intensity;
                        pl.direction = rl.direction;
                        pl.range = rl.range;
                        pl.color = rl.color;
                        pl.cosUmbra = rl.outerCone;
                        pl.cosPenumbra = rl.innerCone;
                        pl.attenuation = { 1.0f, 0.0f, 0.0f };
                        pl.lightType = (rl.type == LightType::Point) ? 1 : 2;
                        pl.shadowIndex = -1;
                        pl.viewProjection = MatrixIdentity();
                    }
                }
            }
            device_->UnmapBuffer(light_cb_[idx]);
        }
        {
            static int s_dbg = 0;
            if (s_dbg < 2) {
                auto* lb = static_cast<const rhi::ForwardLightBuffer*>(device_->MapBuffer(light_cb_[idx], 0, 0));
                if (lb) {
                    PRIMAL_LOG_STREAM(Info, "Render") << "[LightBuf] dir=" << lb->directionalLightCount
                              << " punc=" << lb->punctualLightCount;
                    for (u32 d = 0; d < lb->directionalLightCount && d < 4; ++d)
                        PRIMAL_LOG_STREAM(Info, "Render") << " | dir" << d << " col=(" << lb->directionalLights[d].colorAndShadow.x
                                  << "," << lb->directionalLights[d].colorAndShadow.y
                                  << "," << lb->directionalLights[d].colorAndShadow.z
                                  << ") i=" << lb->directionalLights[d].directionAndIntensity.w
                                  << " dirV=(" << lb->directionalLights[d].directionAndIntensity.x
                                  << "," << lb->directionalLights[d].directionAndIntensity.y
                                  << "," << lb->directionalLights[d].directionAndIntensity.z << ")";
                    PRIMAL_LOG_STREAM(Info, "Render") << std::endl;
                    device_->UnmapBuffer(light_cb_[idx]);
                    ++s_dbg;
                }
            }
        }
    }

    // --- Direct rendering (bypass render graph for reliability) ---

    // Pre-fill shadow VP constant buffers (before any render passes)
    for (int c = 0; c < 2; c++) {
        ViewData svd{};
        svd.viewProjection = cached_shadow_vp_[c];
        svd.invViewProjection = Inverse(svd.viewProjection);
        svd.previousViewProjection = svd.viewProjection;
        auto* mapped = static_cast<ViewData*>(device_->MapBuffer(shadow_view_cb_[c]));
        if (mapped) { *mapped = svd; device_->UnmapBuffer(shadow_view_cb_[c]); }
    }

    // T4.6.5 part 15.3: write shadow_global_ds_[c] bindings per-frame on Vulkan.
    // Binding 0 = shadow_view_cb_[c] (cascade-specific VP); 1+2 mirror global_ds_.
    if (device_->GetPlatform() == RHIPlatform::Vulkan) {
        for (int c = 0; c < 2; c++) {
            DescData params[] = {
                {0, DescriptorType::UniformBuffer, shadow_view_cb_[c]},
                {1, DescriptorType::UniformBuffer, scene_cb_[idx]},
            };
            UpdateDesc(device_, shadow_global_ds_[c], params, 2);
            if (pcg_instance_buffer_ != handles::INVALID_RESOURCE) {
                DescData instParam = {2, DescriptorType::StorageBuffer, pcg_instance_buffer_};
                UpdateDesc(device_, shadow_global_ds_[c], &instParam, 1);
            }
        }
    }

    // Pass 1: Shadow cascade 0
#ifndef __EMSCRIPTEN__
    // Shadow pass is skipped on WASM — the Metal path uses a vertex-buffer
    // slot-0 aliasing trick to override the per-cascade VP, which doesn't
    // translate to WebGPU (vertex slot 0 is real vertex data; shadow VP needs
    // to flow through a uniform). Lighting shader sees cleared depth (1.0)
    // and treats all pixels as fully lit. WFC demo renders without shadows.
    {
        RenderPassDesc rpDesc{};
        rpDesc.depthAttachment.texture = shadow_map_[0];
        rpDesc.depthAttachment.loadOp = LoadAction::Clear;
        rpDesc.depthAttachment.storeOp = StoreAction::Store;
        rpDesc.depthAttachment.clearValue = ClearValue{math::v4{1.0f, 0.f, 0.f, 1.f}};
        cmd->BeginRenderPass(rpDesc);

        cmd->BindGraphicsPipeline(shadow_pipeline_);
        cmd->SetViewport({{0, 0}, {2048.0f, 2048.0f}, 0, 1});
        cmd->SetScissor({{0, 0}, {2048, 2048}});
        // T4.6.5 part 15.3: Vulkan uses shadow_global_ds_[0] (binding 0 pre-set to
        // shadow_view_cb_[0]). Metal keeps global_ds_[idx] + BindVertexBuffers override.
        const DescriptorSetHandle globalSets[] = {
            (device_->GetPlatform() == RHIPlatform::Vulkan) ? shadow_global_ds_[0] : global_ds_[idx]
        };
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadow_layout_, 0, 1, globalSets, 0, nullptr);

        // Metal-only: override vertex slot 0 with dedicated shadow VP CB for this cascade.
        if (device_->GetPlatform() != RHIPlatform::Vulkan) {
            u64 zeroOffset = 0;
            cmd->BindVertexBuffers(0, 1, &shadow_view_cb_[0], &zeroOffset);
        }

        RenderDynamicInstances(cmd, cached_shadow_vp_[0], idx, true);

        cmd->EndRenderPass();
    }

    // Pass 2: Shadow cascade 1
    {
        RenderPassDesc rpDesc{};
        rpDesc.depthAttachment.texture = shadow_map_[1];
        rpDesc.depthAttachment.loadOp = LoadAction::Clear;
        rpDesc.depthAttachment.storeOp = StoreAction::Store;
        rpDesc.depthAttachment.clearValue = ClearValue{math::v4{1.0f, 0.f, 0.f, 1.f}};
        cmd->BeginRenderPass(rpDesc);

        cmd->BindGraphicsPipeline(shadow_pipeline_);
        cmd->SetViewport({{0, 0}, {2048.0f, 2048.0f}, 0, 1});
        cmd->SetScissor({{0, 0}, {2048, 2048}});
        const DescriptorSetHandle globalSets[] = {
            (device_->GetPlatform() == RHIPlatform::Vulkan) ? shadow_global_ds_[1] : global_ds_[idx]
        };
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadow_layout_, 0, 1, globalSets, 0, nullptr);

        // Metal-only: override vertex slot 0 with dedicated shadow VP CB for this cascade.
        if (device_->GetPlatform() != RHIPlatform::Vulkan) {
            u64 zeroOffset = 0;
            cmd->BindVertexBuffers(0, 1, &shadow_view_cb_[1], &zeroOffset);
        }

        RenderDynamicInstances(cmd, cached_shadow_vp_[1], idx, true);

        cmd->EndRenderPass();
    }
#endif // !__EMSCRIPTEN__

    // Pass 3: GBuffer
    {
        RenderPassDesc rpDesc{};
        rpDesc.colorAttachments.resize(4);
        rpDesc.colorAttachments[0].texture = gbuffer_albedo_[idx];
        rpDesc.colorAttachments[0].loadOp = LoadAction::Clear;
        rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
        rpDesc.colorAttachments[1].texture = gbuffer_normal_[idx];
        rpDesc.colorAttachments[1].loadOp = LoadAction::Clear;
        rpDesc.colorAttachments[1].storeOp = StoreAction::Store;
        rpDesc.colorAttachments[2].texture = gbuffer_orm_[idx];
        rpDesc.colorAttachments[2].loadOp = LoadAction::Clear;
        rpDesc.colorAttachments[2].storeOp = StoreAction::Store;
        rpDesc.colorAttachments[3].texture = gbuffer_velocity_[idx];
        rpDesc.colorAttachments[3].loadOp = LoadAction::Clear;
        rpDesc.colorAttachments[3].storeOp = StoreAction::Store;
        rpDesc.depthAttachment.texture = gbuffer_depth_[idx];
        rpDesc.depthAttachment.loadOp = LoadAction::Clear;
        rpDesc.depthAttachment.storeOp = StoreAction::Store;
        rpDesc.depthAttachment.clearValue = ClearValue{math::v4{1.0f, 0.f, 0.f, 1.f}};
        cmd->BeginRenderPass(rpDesc);

        // Pipeline is now bound per-technique inside RenderDynamicInstances
        cmd->SetViewport({{0, 0}, {(float)render_width_, (float)render_height_}, 0, 1});
        cmd->SetScissor({{0, 0}, {render_width_, render_height_}});
        const DescriptorSetHandle globalSets[] = {global_ds_[idx]};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbuffer_layout_, 0, 1, globalSets, 0, nullptr);

        // Unified rendering: all meshes (static + PCG) through RenderDynamicInstances
        RenderDynamicInstances(cmd, MatrixIdentity(), idx, false);

        cmd->EndRenderPass();
    }

    // Pass 3b: Streaming meshes (Phase 9.3b Task 12)
    // Draw GPU-resident StreamingMesh records (GlobalSDFMeshNode output) into
    // the GBuffer attachments. Must run AFTER Pass 3 (so attachments have a
    // valid depth buffer) and BEFORE Pass 4 (so deferred lighting illuminates
    // the streaming surface). Loads existing color+depth, no clear.
    RenderStreamingMeshes(cmd, idx);

    // T4.6.5 part 8 Path B: per-frame UBO fill for compute lighting dispatch.
    // GlobalShaderData (480B) + ForwardLightBuffer (25808B) mirror the engine
    // UBO layout that DeferredLighting.spv expects at bindings 9 and 10.
    if (device_->GetPlatform() == RHIPlatform::Vulkan) {
        math::m4x4 viewInv = Inverse(view_matrix);
        math::v3 cameraDir = math::xyz(math::column(viewInv, 2));

        if (auto* frameData = static_cast<rhi::GlobalShaderData*>(lighting_global_mapped_[idx])) {
            frameData->view = view_matrix;
            frameData->projection = proj_matrix;
            frameData->viewProjection = proj_matrix * view_matrix;
            frameData->previousViewProjection = proj_matrix * view_matrix;
            frameData->invProjection = Inverse(proj_matrix);
            frameData->invViewProjection = Inverse(frameData->viewProjection);
            frameData->cameraPositionAndViewWidth = {camera_position.x, camera_position.y, camera_position.z, static_cast<float>(render_width_)};
            frameData->cameraDirectionAndViewHeight = {cameraDir.x, cameraDir.y, cameraDir.z, static_cast<float>(render_height_)};
            frameData->numDirectionalLights = 0;
            frameData->numPunctualLights = 0;
            frameData->deltaTime = 0.016f;
            frameData->frameCount = 0.0f;
            frameData->renderMode = ibl_ready_ ? 2u : 0u;  // 2 = ShadowAndIBL
            frameData->enableIBL = ibl_ready_ ? 1u : 0u;
            frameData->enableDDGI = 0u;
            frameData->jitterOffset = math::v2{0.0f, 0.0f};
            frameData->debug_directLightBoost = 2.0f;
            frameData->debug_iblStrength = 0.2f;
            frameData->debug_ddgiIndirectWeight = 1.0f;
            frameData->debug_exposure = 1.8f;
        }

        if (auto* lightBuf = static_cast<rhi::ForwardLightBuffer*>(lighting_light_mapped_[idx])) {
            lightBuf->directionalLightCount = 0;
            lightBuf->punctualLightCount = 0;

            if (render_scene_) {
                for (const auto& rl : render_scene_->GetLights()) {
                    if (rl.type == LightType::Directional) {
                        if (lightBuf->directionalLightCount < 4) {
                            auto& dl = lightBuf->directionalLights[lightBuf->directionalLightCount++];
                            dl.viewProjections[0] = cached_shadow_vp_[0];
                            dl.viewProjections[1] = cached_shadow_vp_[1];
                            dl.viewProjections[2] = cached_shadow_vp_[0];
                            dl.viewProjections[3] = cached_shadow_vp_[1];
                            dl.splits = {0.1f, 50.0f, 200.0f, 1000.0f};
                            dl.directionAndIntensity = {rl.direction.x, rl.direction.y, rl.direction.z, rl.intensity};
                            dl.colorAndShadow = {rl.color.x, rl.color.y, rl.color.z, 1.0f};
                        }
                    } else {
                        if (lightBuf->punctualLightCount < 128) {
                            auto& pl = lightBuf->lights[lightBuf->punctualLightCount++];
                            pl.position = rl.position;
                            pl.intensity = rl.intensity;
                            pl.direction = rl.direction;
                            pl.range = rl.range;
                            pl.color = rl.color;
                            pl.cosUmbra = rl.outerCone;
                            pl.cosPenumbra = rl.innerCone;
                            pl.attenuation = {1.0f, 0.0f, 0.0f};
                            pl.lightType = (rl.type == LightType::Point) ? 1 : 2;
                            pl.shadowIndex = -1;
                            pl.viewProjection = MatrixIdentity();
                        }
                    }
                }
            }

            // ForwardShaderData.numDirectionalLights / numPunctualLights are read
            // from the ForwardLightBuffer header (bindings 10), but GlobalShaderData
            // also has these counts — sync them for any shader that reads from GSD.
            if (auto* frameData = static_cast<rhi::GlobalShaderData*>(lighting_global_mapped_[idx])) {
                frameData->numDirectionalLights = lightBuf->directionalLightCount;
                frameData->numPunctualLights = lightBuf->punctualLightCount;
            }
        }
    }

    // Pass 4: Deferred Lighting
    if (device_->GetPlatform() == RHIPlatform::Vulkan) {
        // T4.6.5 part 7 Path B: compute dispatch using existing .spv. Writes
        // HDR output via OpImageStore (no render pass, no Draw).
        // Bindings match Engine/Graphics/Vulkan/shaders/DeferredLighting.spv.
        //
        // T4.6.5 part 9: explicit layout transitions for lighting_output_.
        // StorageImage descriptor (binding 11) requires GENERAL layout — fresh
        // texture starts Unknown, and previous frame's Blit left it in
        // SHADER_READ_ONLY. After Dispatch, transition back to ShaderResource
        // so Pass 4b (Forward Transparency render pass) or Pass 5 (Blit
        // sample) can read it.
        ResourceBarrier toUA{};
        toUA.resource = lighting_output_[idx];
        toUA.beforeState = ResourceState::ShaderResource;
        toUA.afterState = ResourceState::UnorderedAccess;
        toUA.subresource = 0xFFFFFFFF;
        toUA.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&toUA, 1);

        DescData params[] = {
            {0,  DescriptorType::SampledImage,   gbuffer_albedo_[idx]},
            {1,  DescriptorType::SampledImage,   gbuffer_normal_[idx]},
            {2,  DescriptorType::SampledImage,   gbuffer_orm_[idx]},
            {3,  DescriptorType::SampledImage,   gbuffer_velocity_[idx]},
            {4,  DescriptorType::SampledImage,   shadow_map_[0]},
            {5,  DescriptorType::SampledImage,   ibl_ready_ ? irradiance_map_ : black_cube_texture_},
            {6,  DescriptorType::SampledImage,   ibl_ready_ ? prefiltered_map_ : black_cube_texture_},
            {7,  DescriptorType::SampledImage,   ibl_ready_ ? brdf_lut_ : white_texture_},
            {8,  DescriptorType::Sampler,        static_cast<ResourceHandle>(default_sampler_)},
            {9,  DescriptorType::UniformBuffer,  lighting_global_ubos_[idx]},
            {10, DescriptorType::UniformBuffer,  lighting_light_ubos_[idx]},
            {11, DescriptorType::StorageImage,   lighting_output_[idx]},
        };
        UpdateDesc(device_, lighting_compute_ds_[idx], params, 12);

        cmd->BindComputePipeline(lighting_pipeline_);
        const DescriptorSetHandle sets[] = {lighting_compute_ds_[idx]};
        cmd->BindDescriptorSets(PipelineBindPoint::Compute, lighting_compute_layout_, 0, 1, sets, 0, nullptr);
        cmd->Dispatch((render_width_ + 7) / 8, (render_height_ + 7) / 8, 1);

        ResourceBarrier toSR{};
        toSR.resource = lighting_output_[idx];
        toSR.beforeState = ResourceState::UnorderedAccess;
        toSR.afterState = ResourceState::ShaderResource;
        toSR.subresource = 0xFFFFFFFF;
        toSR.queueFamily = 0xFFFFFFFF;
        cmd->InsertBarrier(&toSR, 1);
    } else {
        constexpr DescriptorType depthType =
#ifdef __EMSCRIPTEN__
            DescriptorType::SampledDepthImage;
#else
            DescriptorType::SampledImage;
#endif
        DescData params[] = {
            {0, DescriptorType::UniformBuffer, view_cb_[idx]},
            {1, DescriptorType::UniformBuffer, scene_cb_[idx]},
            {2, DescriptorType::SampledImage, gbuffer_albedo_[idx]},
            {3, DescriptorType::SampledImage, gbuffer_normal_[idx]},
            {4, DescriptorType::SampledImage, gbuffer_orm_[idx]},
            {5, depthType,                    gbuffer_depth_[idx]},
            {6, DescriptorType::SampledImage, shadow_map_[0]},
            {7, DescriptorType::SampledImage, shadow_map_[1]},
            {8, DescriptorType::SampledImage, ibl_ready_ ? irradiance_map_ : black_cube_texture_},
            {9, DescriptorType::SampledImage, ibl_ready_ ? prefiltered_map_ : black_cube_texture_},
            {10, DescriptorType::SampledImage, ibl_ready_ ? brdf_lut_ : white_texture_},
            {11, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
            {12, DescriptorType::Sampler, static_cast<ResourceHandle>(brdf_sampler_)},
            {13, DescriptorType::StorageBuffer, light_cb_[idx]},
        };
        UpdateDesc(device_, lighting_ds_[idx], params, 14);

        RenderPassDesc rpDesc{};
        rpDesc.colorAttachments.resize(1);
        rpDesc.colorAttachments[0].texture = lighting_output_[idx];
        rpDesc.colorAttachments[0].loadOp = LoadAction::Clear;
        rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
        rpDesc.colorAttachments[0].clearValue = ClearValue{math::v4{0.0f, 0.0f, 1.0f, 1.0f}};
        cmd->BeginRenderPass(rpDesc);

        cmd->BindGraphicsPipeline(lighting_pipeline_);
        cmd->SetViewport({{0, 0}, {(float)render_width_, (float)render_height_}, 0, 1});
        cmd->SetScissor({{0, 0}, {render_width_, render_height_}});
        const DescriptorSetHandle sets[] = {lighting_ds_[idx]};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, lighting_layout_, 0, 1, sets, 0, nullptr);
        cmd->Draw(3, 0, 1, 0);
        cmd->EndRenderPass();
    }

    // Pass 4a: Skybox — fills background pixels (depth == 1) with the HDR
    // environment cube. Runs after deferred lighting (which clears the sky to
    // black) and before forward transparency. Sky fragments sit exactly on
    // the far plane, so the pipeline's LessEqual test passes only where no
    // geometry was drawn; depth write is disabled.
    if (skybox_enabled_ && env_cube_ != handles::INVALID_RESOURCE && skybox_pipeline_ != handles::INVALID_PIPELINE) {
        DescData skyParams[] = {
            {0, DescriptorType::UniformBuffer, view_cb_[idx]},
            {1, DescriptorType::UniformBuffer, scene_cb_[idx]},
            {2, DescriptorType::SampledImage, env_cube_},
            {3, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
        };
        UpdateDesc(device_, skybox_ds_[idx], skyParams, 4);

        RenderPassDesc rpDesc{};
        rpDesc.colorAttachments.resize(1);
        rpDesc.colorAttachments[0].texture = lighting_output_[idx];
        rpDesc.colorAttachments[0].loadOp = LoadAction::Load;
        rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
        rpDesc.depthAttachment.texture = gbuffer_depth_[idx];
        rpDesc.depthAttachment.loadOp = LoadAction::Load;
        rpDesc.depthAttachment.storeOp = StoreAction::DontCare;
        cmd->BeginRenderPass(rpDesc);

        cmd->BindGraphicsPipeline(skybox_pipeline_);
        cmd->SetViewport({{0, 0}, {(float)render_width_, (float)render_height_}, 0, 1});
        cmd->SetScissor({{0, 0}, {render_width_, render_height_}});
        const DescriptorSetHandle skySets[] = {skybox_ds_[idx]};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, skybox_layout_, 0, 1, skySets, 0, nullptr);
        cmd->Draw(36, 0, 1, 0); // cube vertices generated from vertexID
        cmd->EndRenderPass();
    }

    // Pass 4b: Forward Transparency (Water/Transparent over deferred lighting)
    {
        // Check if any Water/Transparent instances exist
        bool has_forward = false;
        if (render_scene_) {
            for (auto& proxy : render_scene_->GetProxies()) {
                u32 t = static_cast<u32>(proxy.technique);
                if (t == 3 || t == 4) { has_forward = true; break; }
            }
        }
        if (has_forward) {
            RenderPassDesc rpDesc{};
            rpDesc.colorAttachments.resize(1);
            rpDesc.colorAttachments[0].texture = lighting_output_[idx];
            rpDesc.colorAttachments[0].loadOp = LoadAction::Load;
            rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
            rpDesc.depthAttachment.texture = gbuffer_depth_[idx];
            rpDesc.depthAttachment.loadOp = LoadAction::Load;
            rpDesc.depthAttachment.storeOp = StoreAction::DontCare;
            cmd->BeginRenderPass(rpDesc);

            cmd->SetViewport({{0, 0}, {(float)render_width_, (float)render_height_}, 0, 1});
            cmd->SetScissor({{0, 0}, {render_width_, render_height_}});
            const DescriptorSetHandle globalSets[] = {global_ds_[idx]};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbuffer_layout_, 0, 1, globalSets, 0, nullptr);

            RenderDynamicInstances(cmd, MatrixIdentity(), idx, false, true);

            cmd->EndRenderPass();
        }
    }

    // Pass 4c: Particles (T4.6.5 part 38). Blend additive particles over the
    // deferred-lit scene before Blit. ParticlePass internally gates on
    // active_count==0 (ParticlePass.cpp:336), so zero-particle frames are no-op.
#ifndef DISABLE_PARTICLE_SYSTEM
    RenderParticlePass(cmd, idx, view_matrix, proj_matrix);
#endif

    // Pass 5: Blit to backbuffer
    {
        DescData params[] = {
            {0, DescriptorType::SampledImage, lighting_output_[idx]},
            {1, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
        };
        UpdateDesc(device_, blit_ds_[idx], params, 2);

        RenderPassDesc rpDesc{};
        rpDesc.colorAttachments.resize(1);
        rpDesc.colorAttachments[0].texture = backbuffer;
        rpDesc.colorAttachments[0].loadOp = LoadAction::Clear;
        rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
        rpDesc.colorAttachments[0].clearValue = ClearValue{math::v4{1.0f, 0.0f, 0.0f, 1.0f}};
        cmd->BeginRenderPass(rpDesc);

        cmd->BindGraphicsPipeline(blit_pipeline_);
        cmd->SetViewport({{0, 0}, {(float)render_width_, (float)render_height_}, 0, 1});
        cmd->SetScissor({{0, 0}, {render_width_, render_height_}});
        const DescriptorSetHandle sets[] = {blit_ds_[idx]};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, blit_layout_, 0, 1, sets, 0, nullptr);
        cmd->Draw(3, 0, 1, 0);
        cmd->EndRenderPass();
    }

    // Pass 6: Geometry line overlay + EngineDLL debug draw lines
    // EngineDLL 的 DebugDrawAPI 把 line 写入 debug_draw::queue()，
    // 这里每帧 drain 到 frame_local_debug_lines_，转成 position-only
    // 数组喂给 LineBatchRenderer。color 当前丢弃（shader 单色）。
    frame_local_debug_lines_.clear();
    debug_draw::drain_into(frame_local_debug_lines_);

    const bool has_debug_lines = !frame_local_debug_lines_.empty();
    if (!geometry_entity_ids_.empty() || has_debug_lines) {
        line_renderer_.BeginFrame();
        for (auto eid : geometry_entity_ids_) {
            auto geom = geometry::component::get(game_entity::entity_id{eid});
            if (!geom.is_valid()) continue;
            const auto& pts = geom.tessellate();
            if (pts.size() < 2) continue;
            line_renderer_.AddLines(pts.data(), static_cast<u32>(pts.size()), 0x33FF66u);
        }
        if (has_debug_lines) {
            for (const auto& l : frame_local_debug_lines_) {
                line_renderer_.AddLine(l.a, l.b, l.rgb);
            }
        }

        RenderPassDesc rpDesc{};
        rpDesc.colorAttachments.resize(1);
        rpDesc.colorAttachments[0].texture = backbuffer;
        rpDesc.colorAttachments[0].loadOp = LoadAction::Load;
        rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
        rpDesc.depthAttachment.texture = gbuffer_depth_[idx];
        rpDesc.depthAttachment.loadOp = LoadAction::Load;
        rpDesc.depthAttachment.storeOp = StoreAction::DontCare;
        cmd->BeginRenderPass(rpDesc);

        cmd->SetViewport({{0, 0}, {(float)render_width_, (float)render_height_}, 0, 1});
        cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

        math::m4x4 viewProj = proj_matrix * view_matrix;
        line_renderer_.Render(cmd, viewProj, backbuffer_format);

        cmd->EndRenderPass();
    }
}

// T4.6.5 part 38: ParticlePass runtime wiring. Blends additive particles over
// the deferred-lit scene (lighting_output_[idx]) before Pass 5 Blit. Caller
// gates by check that ParticleSystem has emitted at least one particle this
// frame; ParticlePass::execute additionally early-returns on active_count==0.
// ParticlePass does NOT begin its own render pass — caller wraps it.
#ifndef DISABLE_PARTICLE_SYSTEM
void ForwardSceneRenderer::RenderParticlePass(rhi::RHICommandBuffer* cmd, u32 frame_index,
                                              const math::m4x4& view_matrix,
                                              const math::m4x4& proj_matrix) {
    if (!initialized_) return;
    u32 idx = frame_index % 3;

    RenderPassDesc rpDesc{};
    rpDesc.colorAttachments.resize(1);
    rpDesc.colorAttachments[0].texture = lighting_output_[idx];
    rpDesc.colorAttachments[0].loadOp  = LoadAction::Load;
    rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
    rpDesc.depthAttachment.texture  = gbuffer_depth_[idx];
    rpDesc.depthAttachment.loadOp   = LoadAction::Load;
    rpDesc.depthAttachment.storeOp  = StoreAction::DontCare;
    cmd->BeginRenderPass(rpDesc);
    cmd->SetViewport({{0, 0}, {(float)render_width_, (float)render_height_}, 0, 1});
    cmd->SetScissor ({{0, 0}, {render_width_, render_height_}});

#ifndef DISABLE_PARTICLE_FX
    // PFX 渲染兼容桥：喂入按混合模式分桶的 PFX 粒子缓冲（§7-N2 前置）
    u32 pfx_total = 0;
    u32 pfx_blend_counts[4] = { 0, 0, 0, 0 };
    const auto* pfx_data = primal::pfx::packed_frame_pool(idx, &pfx_total, pfx_blend_counts);
    particle_pass_.set_pfx_data(pfx_data, pfx_total, pfx_blend_counts);

    // mesh 渲染器：renderer_type=mesh 的发射器 → 48B 实例（几何源 = instance 声明
    // 或内置立方体）。同发射器实例同槽且连续，pass 侧扫描分组绘制。
    {
        using primal::pfx::render::mesh_instance_data;
        using RendererType = primal::pfx::emitter_program;
        static thread_local mesh_instance_data mesh_buf[ParticleFXMeshPass::max_instances()];
        static thread_local u32 mesh_slots[ParticleFXMeshPass::max_instances()];
        u32 mesh_count = 0;
        primal::pfx::for_each_emitter([&](primal::pfx::pfx_emitter& em) {
            if (mesh_count >= ParticleFXMeshPass::max_instances()) return;
            if (em.editor_preview()) return;   // 编辑器私有预览不进世界场景
            if (!em.has_program() ||
                em.program().renderer_type != RendererType::renderer_mesh) return;

            // instance 声明的几何源 → 槽号（懒加载注册；失败回退立方体并缓存）。
            // "builtin:cube" / "builtin:sphere" 为内置程序化几何伪路径。
            u32 slot = 0;
            const std::string& mesh_path = em.program().instance_mesh_path;
            if (!mesh_path.empty() && mesh_path != "builtin:cube") {
                if (mesh_path == "builtin:sphere") {
                    slot = ParticleFXMeshPass::builtin_sphere_slot();
                } else if (auto it = pfx_instance_geometry_slots_.find(mesh_path);
                           it != pfx_instance_geometry_slots_.end()) {
                    slot = it->second;
                } else {
                    primal::pfx::render::instance_geometry geom;
                    std::string geom_err;
                    if (primal::pfx::render::load_instance_geometry(mesh_path.c_str(), geom,
                                                                    geom_err)) {
                        slot = particle_fx_mesh_pass_.register_geometry(
                            geom.positions.data(), geom.normals.data(),
                            (u32)geom.positions.size(), geom.indices.data(), geom.index_count);
                        if (slot == u32_invalid_id) slot = 0;   // 槽满等 → 立方体回退
                    } else {
                        PRIMAL_LOG_STREAM(Warn, "ParticleFX")
                            << "instance mesh '" << mesh_path << "' load failed (" << geom_err
                            << "), using cube fallback" << std::endl;
                        slot = 0;
                    }
                    pfx_instance_geometry_slots_.emplace(mesh_path, slot);
                }
            }

            const u32 base = mesh_count;
            mesh_count += primal::pfx::render::pack_mesh_instances(
                em.store(), mesh_buf + mesh_count,
                ParticleFXMeshPass::max_instances() - mesh_count);
            for (u32 i = base; i < mesh_count; ++i) mesh_slots[i] = slot;
        });
        particle_fx_mesh_pass_.stage_instances(mesh_buf, mesh_slots, mesh_count, idx);
    }
#endif

    particle_pass_.execute(cmd, frame_index, view_matrix, proj_matrix);

#ifndef DISABLE_PARTICLE_FX
    particle_fx_mesh_pass_.execute(cmd, idx, view_matrix, proj_matrix);
#endif

#ifndef DISABLE_PARTICLE_FX
    // §7-N6 light 渲染器：亮粒子 → 加色 HDR billboard 灯（同一 lighting 输出）
    {
        constexpr u32 kMaxLights = primal::graphics::ParticleFXLightPass::max_lights();
        float staged[kMaxLights * 8] = {};
        u32 n = 0;
        primal::pfx::for_each_emitter([&](primal::pfx::pfx_emitter& em) {
            if (em.editor_preview()) return;   // 编辑器私有预览不进世界场景
            auto& store = em.store();
            const u32 capacity = store.capacity();
            if (store.alive_count() == 0 || n >= kMaxLights) return;
            const math::v4* pos = store.v4_channel(primal::pfx::attr::position);
            const math::v4* col = store.v4_channel(primal::pfx::attr::color);
            if (!pos || !col) return;
            for (u32 row = 0; row < capacity && n < kMaxLights; ++row) {
                if (!store.is_row_alive(row)) continue;
                float* l = staged + n * 8;
                l[0] = pos[row].x; l[1] = pos[row].y; l[2] = pos[row].z;
                l[3] = col[row].w;                       // intensity = alpha（淡出即灯弱）
                l[4] = col[row].x; l[5] = col[row].y; l[6] = col[row].z; l[7] = 0.f;
                ++n;
            }
        });
        particle_fx_light_pass_.stage_lights(staged, n, idx);
        particle_fx_light_pass_.execute(cmd, idx, view_matrix, proj_matrix);
    }
#endif

    cmd->EndRenderPass();
}
#endif // DISABLE_PARTICLE_SYSTEM

} // namespace primal::graphics
