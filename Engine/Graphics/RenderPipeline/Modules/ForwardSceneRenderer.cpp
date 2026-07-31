#include "ForwardSceneRenderer.h"
#include "Graphics/RHI/Core/RHIMath.h"
#if !defined(__EMSCRIPTEN__)
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
#include "Graphics/Scene/LightSyncSystem.h"
#include "Graphics/DebugDraw/DebugDrawQueue.h"
#include "Content/ContentToEngine.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Geometry.h"
#include "Graphics/Material/ShaderTechnique.h"
#include "Graphics/Material/InstanceData.h"

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
};

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

static const std::string SHADER_DIR =
    "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/Forward/";

static const std::string RHI_SHADER_DIR =
    "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/RHI/Shaders/";

static const std::string VULKAN_SHADER_DIR =
    "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Vulkan/shaders/Forward/";

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
        auto content = ReadFileToString(dir + name);
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
        const char* stageSuffix = (stage == ShaderStage::Vertex) ? ".vert" : ".frag";
        std::string path = VULKAN_SHADER_DIR + filename + stageSuffix + ".spv";
        auto bytes = ReadFileToBytes(path);
        if (bytes.empty()) {
            std::cerr << "[ForwardSceneRenderer] Failed to load SPIR-V: " << path << std::endl;
            return {};
        }
        return bytes;
    }

    std::string path = SHADER_DIR + filename + std::string(".metal");
    std::string source = ReadFileToString(path);
    if (source.empty()) {
        std::cerr << "[ForwardSceneRenderer] Failed to load shader: " << filename << std::endl;
        return {};
    }
    std::set<std::string> included;
    source = InlineIncludes(source, 0, included);

    return std::vector<u8>(source.begin(), source.end());
}

static void WriteTextureImmediate(RHIDeviceBase* device, ResourceHandle texture,
                                   const void* data, u64 size, u32 w, u32 h, u32 layer) {
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
        std::cerr << "[ForwardSceneRenderer] stbi_load failed: " << path << std::endl;
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

std::string ResolveTexturePath(const std::string& baseDir, const std::string& filename) {
    if (filename.empty()) return "";
    std::string cleanName = filename;
    std::replace(cleanName.begin(), cleanName.end(), '\\', '/');

    std::vector<std::string> basePaths = {
        baseDir,
        baseDir + "fbx_textures/",
        baseDir + "models/Sponza/",
    };
    for (const auto& base : basePaths) {
        std::string fullPath = base + cleanName;
        std::ifstream f(fullPath.c_str());
        if (f.good()) return fullPath;
    }
    return baseDir + cleanName;
}

} // anonymous namespace

// ============================================================================
// ForwardSceneRenderer Implementation
// ============================================================================

ForwardSceneRenderer::~ForwardSceneRenderer() { Shutdown(); }

bool ForwardSceneRenderer::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height) {
    if (initialized_) return true;

    // T4.6.5: ForwardSceneRenderer still deferred on Vulkan.
    // T4.6.5 part 1: platform-aware loader (.spv binary on Vulkan).
    // T4.6.5 part 2: stage-suffix naming + "main" entry convention. DepthOnly +
    //                Skybox ports verified loading silently.
    // T4.6.5 part 3 (this commit): Path A blit — Blit.vert/Blit.frag authored
    //                + blit_set_layout_ gained Sampler binding + load() now
    //                picks "Blit" file on Vulkan. Lighting pipeline conversion
    //                to compute (Path B) still pending.
    //
    // Remaining blockers (multi-session scope):
    //   1. Path B lighting compute conversion: lighting_pipeline_ still loads
    //      DeferredLighting vertexMain/fragmentLighting_v3 as vert/frag, but
    //      only a GLCompute .spv exists. Need to swap to CreateComputePipeline
    //      + Dispatch at the bind site.
    //   2. 8 of 11 ForwardSceneRenderer shaders still have no SPIR-V port:
    //      GBuffer, GBufferAlphaClip, GBufferUnlit, GBufferFoliage, GBufferWater,
    //      GBufferTransparent, ForwardTransparency, StreamingGBuffer
    //   3. Vertex buffer binding slot 0/1 convention differs (Metal uses
    //      [[buffer(1)]] for vertices; Vulkan expects binding 0).
    // Keep the skip in place until those are resolved.
    if (device && device->GetPlatform() == RHIPlatform::Vulkan) {
        std::cerr << "[ForwardSceneRenderer] Skipped on Vulkan (T4.6.5: "
                     "3/11 shaders ported; Path B lighting + 8 more pending)"
                  << std::endl;
        return false;
    }

    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;

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

        desc.layout = skybox_set_layout_;
        skybox_ds_[i] = device_->CreateDescriptorSet(desc);

        desc.layout = blit_set_layout_;
        blit_ds_[i] = device_->CreateDescriptorSet(desc);
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
#endif

    // Geometry line renderer
    line_renderer_.Initialize(device_);

    initialized_ = true;
    std::cout << "[ForwardSceneRenderer] Initialized (" << render_width << "x" << render_height << ")" << std::endl;
    return true;
}

void ForwardSceneRenderer::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;

    // Geometry line renderer
    line_renderer_.Shutdown();

#ifndef DISABLE_PARTICLE_SYSTEM
    particle_pass_.shutdown();
#endif

    // Destroy pipelines
    for (auto* p : {&gbuffer_pipeline_, &shadow_pipeline_, &lighting_pipeline_,
                    &skybox_pipeline_, &blit_pipeline_, &alphaclip_pipeline_, &unlit_pipeline_,
                    &foliage_pipeline_, &water_pipeline_, &transparent_pipeline_,
                    &forward_water_pipeline_, &forward_transparent_pipeline_,
                    &streaming_pipeline_}) {
        if (*p != handles::INVALID_PIPELINE) { device_->DestroyPipeline(*p); *p = handles::INVALID_PIPELINE; }
    }
    // Destroy layouts
    for (auto* l : {&gbuffer_layout_, &shadow_layout_, &lighting_layout_,
                    &skybox_layout_, &blit_layout_}) {
        if (*l != handles::INVALID_PIPELINE_LAYOUT) { device_->DestroyPipelineLayout(*l); *l = handles::INVALID_PIPELINE_LAYOUT; }
    }
    // Destroy descriptor set layouts
    for (auto* d : {&global_set_layout_, &material_set_layout_, &lighting_set_layout_,
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
    }
    for (int i = 0; i < 2; i++) {
        if (shadow_view_cb_[i] != handles::INVALID_RESOURCE) { device_->DestroyBuffer(shadow_view_cb_[i]); shadow_view_cb_[i] = handles::INVALID_RESOURCE; }
    }
    // Destroy fallback textures
    if (white_texture_ != handles::INVALID_RESOURCE) { device_->DestroyTexture(white_texture_); white_texture_ = handles::INVALID_RESOURCE; }
    if (flat_normal_texture_ != handles::INVALID_RESOURCE) { device_->DestroyTexture(flat_normal_texture_); flat_normal_texture_ = handles::INVALID_RESOURCE; }
    if (black_cube_texture_ != handles::INVALID_RESOURCE) { device_->DestroyTexture(black_cube_texture_); black_cube_texture_ = handles::INVALID_RESOURCE; }
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
    };
    default_sampler_ = device_->CreateSampler(linearWrap);

    SamplerDesc linearClamp{
        .minFilter = FilterMode::Linear, .magFilter = FilterMode::Linear,
        .addressU = TextureAddressMode::Clamp, .addressV = TextureAddressMode::Clamp,
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
        global_set_layout_ = device_->CreateDescriptorSetLayout({2, bindings});
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
    // Lighting set (13 bindings)
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel},
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel},
            {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {5, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {6, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {7, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {8, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {9, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {10, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
            {11, DescriptorType::Sampler, 1, ShaderStage::Pixel},
            {12, DescriptorType::Sampler, 1, ShaderStage::Pixel},
        };
        lighting_set_layout_ = device_->CreateDescriptorSetLayout({13, bindings});
    }
    // Skybox set
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex},
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex},
            {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel},
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
    const RHIPlatform platform = device_->GetPlatform();
    auto load = [this, platform](const char* file, const char* entry, ShaderStage stage) -> ShaderHandle {
        auto src = LoadShaderSource(file, platform, stage);
        if (src.empty()) {
            std::cerr << "[ForwardSceneRenderer] Failed to load shader: " << file << "/" << entry << std::endl;
            return handles::INVALID_SHADER;
        }
        // Vulkan SPIR-V files use "main" entry point; Metal uses named entries.
        const char* vkEntry = "main";
        const char* useEntry = (platform == RHIPlatform::Vulkan) ? vkEntry : entry;
        auto handle = device_->CreateShader(src.data(), src.size(), stage, useEntry);
        if (handle == handles::INVALID_SHADER) {
            std::cerr << "[ForwardSceneRenderer] Failed to compile shader: " << file << "/" << entry << std::endl;
        }
        return handle;
    };

    gbuffer_vs_ = load("GBuffer", "vertexMain", ShaderStage::Vertex);
    gbuffer_ps_ = load("GBuffer", "fragmentMain", ShaderStage::Pixel);
    shadow_vs_ = load("DepthOnly", "shadow_mapping_vs", ShaderStage::Vertex);

    lighting_vs_ = load("DeferredLighting", "vertexMain", ShaderStage::Vertex);
    lighting_ps_ = load("DeferredLighting", "fragmentLighting_v3", ShaderStage::Pixel);
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

    // Forward pass shaders for Water/Transparent (inline PBR, rendered after deferred lighting)
    forward_water_vs_ = load("ForwardTransparency", "forwardWaterVS", ShaderStage::Vertex);
    forward_water_ps_ = load("ForwardTransparency", "forwardWaterFS", ShaderStage::Pixel);
    forward_transparent_vs_ = load("ForwardTransparency", "forwardTransparentVS", ShaderStage::Vertex);
    forward_transparent_ps_ = load("ForwardTransparency", "forwardTransparentFS", ShaderStage::Pixel);

    // Streaming mesh shaders (Phase 9.3b Task 12) — SoA vertex pulling
    streaming_vs_ = load("StreamingGBuffer", "streamingVertexMain", ShaderStage::Vertex);
    streaming_ps_ = load("StreamingGBuffer", "streamingFragmentMain", ShaderStage::Pixel);
}

void ForwardSceneRenderer::CreatePipelines() {
    PushConstantRange modelPush{ShaderStage::Vertex, 0, sizeof(PCGPushConsts)};

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
        gbuffer_pipeline_ = device_->CreateGraphicsPipeline(desc);
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
        shadow_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Lighting
    {
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
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
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
        forward_transparent_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
    // Streaming mesh pipeline — same layout/RTs/depth as GBuffer, different VS/PS
    // (Phase 9.3b Task 12). Reads SoA buffers at slots 20/21 instead of AoS.
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
        streaming_pipeline_ = device_->CreateGraphicsPipeline(desc);
    }
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
    // Zero RGBA16F = no IBL contribution, which is the correct default when SetIBLMaps() is not called.
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
    for (int i = 0; i < 3; i++) {
        auto makeTex = [&](DataFormat fmt, const char* name) -> ResourceHandle {
            TextureDesc desc{};
            desc.size = {render_width_, render_height_, 1};
            desc.format = fmt;
            desc.type = TextureType::Texture2D;
            desc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = name;
            return device_->CreateTexture(desc);
        };

        gbuffer_albedo_[i] = makeTex(DataFormat::BGRA8_UNorm, "FwdAlbedo");
        gbuffer_normal_[i] = makeTex(DataFormat::RGBA16_Float, "FwdNormal");
        gbuffer_orm_[i] = makeTex(DataFormat::BGRA8_UNorm, "FwdORM");
        gbuffer_velocity_[i] = makeTex(DataFormat::RG16_Float, "FwdVelocity");
        gbuffer_depth_[i] = makeTex(DataFormat::D32_Float, "FwdDepth");
        lighting_output_[i] = makeTex(DataFormat::RGBA16_Float, "FwdLighting");
    }
}

// ============================================================================
// Scene Loading
// ============================================================================

bool ForwardSceneRenderer::LoadScene(const std::string& model_path) {
    if (scene_loaded_) UnloadScene();

    std::ifstream file(model_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[ForwardSceneRenderer] Failed to open file: " << model_path << std::endl;
        return false;
    }
    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(file_size);
    if (!file.read(buffer.data(), file_size)) {
        std::cerr << "[ForwardSceneRenderer] Failed to read file: " << model_path << std::endl;
        return false;
    }
    mesh_infos_ = scene_adapter_.LoadRenderItemData(device_, buffer.data(), (u32)buffer.size());
    if (mesh_infos_.empty()) {
        std::cerr << "[ForwardSceneRenderer] Failed to load scene: " << model_path << std::endl;
        return false;
    }

    // Create per-mesh material descriptor sets with loaded textures
    const std::string assetBaseDir =
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";
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
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.diffuseTexturePath);
            if (!fullPath.empty()) {
                auto it = textureCache.find(fullPath);
                if (it != textureCache.end()) {
                    albedo = it->second;
                } else {
                    albedo = LoadTextureFromFile(device_, fullPath, true);
                    if (albedo != handles::INVALID_RESOURCE) {
                        textureCache[fullPath] = albedo;
                    } else {
                        std::cerr << "[ForwardSceneRenderer] Failed to load albedo: "
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
                std::string fullPath = ResolveTexturePath(assetBaseDir, inferred);
                std::ifstream testFile(fullPath);
                if (testFile.good()) meshInfo.normalTexturePath = inferred;
            }
        }

        // Load normal texture
        if (!meshInfo.normalTexturePath.empty()) {
            std::string fullPath = ResolveTexturePath(assetBaseDir, meshInfo.normalTexturePath);
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
    }

    std::cout << "[ForwardSceneRenderer] Loaded scene: " << model_path
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

void ForwardSceneRenderer::SetLightDirection(math::v3 dir) { light_dir_ = dir; }
void ForwardSceneRenderer::SetLightColor(math::v4 color) { light_color_ = color; }
ParticlePass* ForwardSceneRenderer::GetParticlePass() { return &particle_pass_; }

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

    std::cout << "[ForwardSceneRenderer] Created " << result.entity_ids.size()
              << " static ECS entities" << std::endl;
    return result;
}

void ForwardSceneRenderer::DestroyStaticEntities() {
    // Entity cleanup handled by caller via StandardRenderPipeline
}

u32 ForwardSceneRenderer::RegisterMeshResource(id::id_type geometry_content_id,
                                                id::id_type albedo_texture_id,
                                                id::id_type normal_texture_id,
                                                id::id_type orm_texture_id) {
    // Create RenderMesh from content system asset
    RenderMesh* mesh = RenderMesh::CreateFromAsset(device_, geometry_content_id);
    if (!mesh) {
        std::cerr << "[ForwardSceneRenderer] RegisterMeshResource: CreateFromAsset failed for id "
                  << geometry_content_id << std::endl;
        return (u32)-1;
    }

    // Create SceneDataMeshInfo
    SceneDataMeshInfo info{};
    info.mesh = mesh;
    info.meshEntityId = geometry_content_id;
    info.name = "Imported_" + std::to_string(geometry_content_id);

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
    mesh_id_to_index_[geometry_content_id] = slot;

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
                std::cerr << "[ForwardSceneRenderer] Shadow cull: 0/" << render_scene_->GetProxies().size()
                          << " proxies visible. AABB issue?" << std::endl;
                auto& proxies = render_scene_->GetProxies();
                if (!proxies.empty()) {
                    auto& p = proxies[0];
                    std::cerr << "  Proxy 0 AABB: min=(" << p.worldAABB.min.x << "," << p.worldAABB.min.y << "," << p.worldAABB.min.z
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
        std::cout << "[ShadowPass] visible=" << visible.size()
                  << " total_proxies=" << (render_scene_ ? render_scene_->GetProxies().size() : 0)
                  << " mesh_map=" << mesh_id_to_index_.size() << std::endl;
        shadowDebug = false;
    }

    // Group visible proxies by technique then by mesh_infos_ index
    u32 mesh_count = static_cast<u32>(mesh_infos_.size());
    std::array<std::unordered_map<u32, std::vector<graphics::InstanceData>>, TechniqueCount> technique_groups;
    for (auto* proxy : visible) {
        auto it = mesh_id_to_index_.find(proxy->meshId);
        if (it == mesh_id_to_index_.end()) continue;
        u32 meshIdx = it->second;
        if (meshIdx >= mesh_count || !mesh_infos_[meshIdx].mesh) continue;
        u32 t = static_cast<u32>(proxy->technique);
        if (t >= TechniqueCount) t = 0;
        graphics::InstanceData inst{};
        inst.transform = proxy->transform;
        memcpy(inst.base_color, proxy->base_color, sizeof(f32) * 4);
        inst.roughness    = proxy->roughness;
        inst.metallic     = proxy->metallic;
        inst.alpha_cutoff = proxy->alpha_cutoff;
        technique_groups[t][meshIdx].push_back(inst);
    }

    // Check if any groups exist
    bool has_any = false;
    for (u32 t = 0; t < TechniqueCount; ++t) {
        if (!technique_groups[t].empty()) { has_any = true; break; }
    }
    if (!has_any) return;

    // Compute total instance count and ensure buffer capacity
    u32 total = 0;
    for (u32 t = 0; t < TechniqueCount; ++t)
        for (auto& [_, transforms] : technique_groups[t])
            total += (u32)transforms.size();
    u32 required_size = total * sizeof(graphics::InstanceData);
    if (pcg_instance_buffer_capacity_ < required_size) {
        if (pcg_instance_buffer_ != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(pcg_instance_buffer_);
        }
        BufferDesc desc{};
        desc.size = required_size;
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
    u32 off = 0;
    for (u32 t = 0; t < TechniqueCount; ++t) {
        for (auto& [meshIdx, instances] : technique_groups[t]) {
            all_ranges[t][meshIdx] = {off, (u32)instances.size()};
            for (u32 j = 0; j < instances.size(); j++) {
                ((graphics::InstanceData*)buf)[off + j] = instances[j];
            }
            off += (u32)instances.size();
        }
    }
    device_->UnmapBuffer(pcg_instance_buffer_);

    // Select layout based on pass type
    auto layout = shadow_pass ? shadow_layout_ : gbuffer_layout_;

    // Draw each technique bucket in render order
    for (u32 t = 0; t < TechniqueCount; ++t) {
        if (all_ranges[t].empty()) continue;

        // Forward pass: only render Water(3) and Transparent(4)
        if (forward_pass && t != 3 && t != 4) continue;
        // GBuffer pass: skip Water(3) and Transparent(4) — they render in forward pass
        if (!shadow_pass && !forward_pass && (t == 3 || t == 4)) continue;

        // Bind technique-specific pipeline for GBuffer or forward pass
        if (!shadow_pass) {
            if (forward_pass) {
                cmd->BindGraphicsPipeline(GetForwardTechniquePipeline(static_cast<ShaderTechnique>(t)));
            } else {
                cmd->BindGraphicsPipeline(GetTechniquePipeline(static_cast<ShaderTechnique>(t)));
            }
        }

        for (auto& [meshIdx, range] : all_ranges[t]) {
            u64 inst_offset = range.offset * sizeof(graphics::InstanceData);
            cmd->BindVertexBuffers(3, 1, &pcg_instance_buffer_, &inst_offset);
            auto matSet = (meshIdx < material_ds_.size()) ? material_ds_[meshIdx] : material_ds_[0];
            const DescriptorSetHandle matSets[] = {matSet};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, layout, 1, 1, matSets, 0, nullptr);
            PCGPushConsts pc{};
            pc.transform = MatrixIdentity();
            pc.use_instances = 1;
            cmd->PushConstants(layout, ShaderStage::Vertex, 0, sizeof(PCGPushConsts), &pc);
            mesh_infos_[meshIdx].mesh->Draw(cmd, range.count, 0, 20);
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
                std::cout << "[RenderStream] frame_idx=" << frame_index
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

        // Bind positions (20), elements (21), indices (22). The shader does
        // manual indexed drawing — DrawIndirect issues idx_count invocations,
        // and indices[vid] maps each invocation to the actual vertex.
        ResourceHandle vb[3] = { sm.mesh->positions, sm.mesh->elements, sm.mesh->indices };
        u64 offsets[3] = { 0, 0, 0 };
        cmd->BindVertexBuffers(20, 3, vb, offsets);
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
                                   u32 frame_index) {
    if (!initialized_ || !scene_loaded_) return;

    u32 idx = frame_index % 3;

    // Debug: first frame only
    static bool first_frame = true;
    if (first_frame) {
        size_t proxyCount = render_scene_ ? render_scene_->GetProxies().size() : 0;
        std::cout << "[ForwardSceneRenderer::Render] scene_loaded_=" << scene_loaded_
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

    // Pass 1: Shadow cascade 0
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
        const DescriptorSetHandle globalSets[] = {global_ds_[idx]};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadow_layout_, 0, 1, globalSets, 0, nullptr);

        // Override buffer slot 0 with dedicated shadow VP CB for this cascade
        u64 zeroOffset = 0;
        cmd->BindVertexBuffers(0, 1, &shadow_view_cb_[0], &zeroOffset);

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
        const DescriptorSetHandle globalSets[] = {global_ds_[idx]};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, shadow_layout_, 0, 1, globalSets, 0, nullptr);

        // Override buffer slot 0 with dedicated shadow VP CB for this cascade
        u64 zeroOffset = 0;
        cmd->BindVertexBuffers(0, 1, &shadow_view_cb_[1], &zeroOffset);

        RenderDynamicInstances(cmd, cached_shadow_vp_[1], idx, true);

        cmd->EndRenderPass();
    }

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

    // Pass 4: Deferred Lighting
    {
        DescData params[] = {
            {0, DescriptorType::UniformBuffer, view_cb_[idx]},
            {1, DescriptorType::UniformBuffer, scene_cb_[idx]},
            {2, DescriptorType::SampledImage, gbuffer_albedo_[idx]},
            {3, DescriptorType::SampledImage, gbuffer_normal_[idx]},
            {4, DescriptorType::SampledImage, gbuffer_orm_[idx]},
            {5, DescriptorType::SampledImage, gbuffer_depth_[idx]},
            {6, DescriptorType::SampledImage, shadow_map_[0]},
            {7, DescriptorType::SampledImage, shadow_map_[1]},
            {8, DescriptorType::SampledImage, ibl_ready_ ? irradiance_map_ : black_cube_texture_},
            {9, DescriptorType::SampledImage, ibl_ready_ ? prefiltered_map_ : black_cube_texture_},
            {10, DescriptorType::SampledImage, ibl_ready_ ? brdf_lut_ : white_texture_},
            {11, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
            {12, DescriptorType::Sampler, static_cast<ResourceHandle>(brdf_sampler_)},
        };
        UpdateDesc(device_, lighting_ds_[idx], params, 13);

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
            line_renderer_.AddLines(pts.data(), static_cast<u32>(pts.size()));
        }
        if (has_debug_lines) {
            // 转成紧凑 position 数组 —— color (rgb) 暂时不传。
            frame_debug_positions_.clear();
            frame_debug_positions_.reserve(frame_local_debug_lines_.size() * 2);
            for (const auto& l : frame_local_debug_lines_) {
                frame_debug_positions_.push_back(l.a);
                frame_debug_positions_.push_back(l.b);
            }
            line_renderer_.AddLines(frame_debug_positions_.data(),
                                    static_cast<u32>(frame_debug_positions_.size()));
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
        line_renderer_.Render(cmd, viewProj);

        cmd->EndRenderPass();
    }
}

} // namespace primal::graphics
