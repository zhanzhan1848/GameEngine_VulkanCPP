#include "ForwardSceneRenderer.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalTexture.h"
#include "Graphics/RHI/Platforms/Metal/MetalPipeline.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Content/ContentToEngine.h"
#include "Components/Entity.h"
#include "Components/Transform.h"
#include "Components/Geometry.h"

#include <fstream>
#include <sstream>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <set>
#include <unordered_map>
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
    math::v2 padding;
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

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
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

static std::vector<u8> LoadShaderSource(const char* filename) {
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
                    &skybox_pipeline_, &blit_pipeline_}) {
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
        };
        blit_set_layout_ = device_->CreateDescriptorSetLayout({1, bindings});
    }
}

void ForwardSceneRenderer::CreateShaders() {
    auto load = [this](const char* file, const char* entry, ShaderStage stage) -> ShaderHandle {
        auto src = LoadShaderSource(file);
        if (src.empty()) {
            std::cerr << "[ForwardSceneRenderer] Failed to load shader: " << file << "/" << entry << std::endl;
            return handles::INVALID_SHADER;
        }
        auto handle = device_->CreateShader(src.data(), src.size(), stage, entry);
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
    blit_vs_ = load("DeferredLighting", "vertexMain", ShaderStage::Vertex);
    blit_ps_ = load("DeferredLighting", "fragmentBlit", ShaderStage::Pixel);

    skybox_vs_ = load("Skybox", "vertexSkybox", ShaderStage::Vertex);
    skybox_ps_ = load("Skybox", "fragmentSkybox", ShaderStage::Pixel);
}

void ForwardSceneRenderer::CreatePipelines() {
    PushConstantRange modelPush{ShaderStage::Vertex, 2, sizeof(math::m4x4)};

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
                                                    bool shadow_pass) {
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

    // Group visible proxies by mesh_infos_ index
    u32 mesh_count = static_cast<u32>(mesh_infos_.size());
    std::unordered_map<u32, std::vector<math::m4x4>> groups;
    for (auto* proxy : visible) {
        auto it = mesh_id_to_index_.find(proxy->meshId);
        if (it == mesh_id_to_index_.end()) continue;
        u32 meshIdx = it->second;
        if (meshIdx >= mesh_count || !mesh_infos_[meshIdx].mesh) continue;
        math::m4x4 xform = proxy->transform;
        groups[meshIdx].push_back(xform);
    }
    if (groups.empty()) return;

    // Compute total instance count and ensure buffer capacity
    u32 total = 0;
    for (auto& [_, transforms] : groups) total += (u32)transforms.size();
    u32 required_size = total * sizeof(math::m4x4);
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

    // Compute group offsets
    struct GroupRange { u32 offset; u32 count; };
    std::unordered_map<u32, GroupRange> group_ranges;
    u32 off = 0;
    for (auto& [meshIdx, transforms] : groups) {
        group_ranges[meshIdx] = {off, (u32)transforms.size()};
        for (u32 j = 0; j < transforms.size(); j++) {
            ((math::m4x4*)buf)[off + j] = transforms[j];
        }
        off += (u32)transforms.size();
    }
    device_->UnmapBuffer(pcg_instance_buffer_);

    // Select layout based on pass type
    auto layout = shadow_pass ? shadow_layout_ : gbuffer_layout_;

    // Draw each group
    for (auto& [meshIdx, range] : group_ranges) {
        u64 inst_offset = range.offset * sizeof(math::m4x4);
        cmd->BindVertexBuffers(3, 1, &pcg_instance_buffer_, &inst_offset);
        auto matSet = (meshIdx < material_ds_.size()) ? material_ds_[meshIdx] : material_ds_[0];
        const DescriptorSetHandle matSets[] = {matSet};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, layout, 1, 1, matSets, 0, nullptr);
        PCGPushConsts pc{};
        pc.transform = MatrixIdentity();
        pc.use_instances = 1;
        cmd->PushConstants(layout, ShaderStage::Vertex, 2, sizeof(PCGPushConsts), &pc);
        mesh_infos_[meshIdx].mesh->Draw(cmd, range.count, 0, 20);
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

    // Update shadow VPs — match TestParticleSponza light setup
    // lightPos = {100, 150, 50, 0} → lightDir = Normalize(-lightPos)
    math::v3 lightPos{100.0f, 150.0f, 50.0f};
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
        sd.lightColor = light_color_;
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

        cmd->BindGraphicsPipeline(gbuffer_pipeline_);
        cmd->SetViewport({{0, 0}, {(float)render_width_, (float)render_height_}, 0, 1});
        cmd->SetScissor({{0, 0}, {render_width_, render_height_}});
        const DescriptorSetHandle globalSets[] = {global_ds_[idx]};
        cmd->BindDescriptorSets(PipelineBindPoint::Graphics, gbuffer_layout_, 0, 1, globalSets, 0, nullptr);

        // Unified rendering: all meshes (static + PCG) through RenderDynamicInstances
        RenderDynamicInstances(cmd, MatrixIdentity(), idx, false);

        cmd->EndRenderPass();
    }

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

    // Pass 5: Blit to backbuffer
    {
        DescData params[] = {
            {0, DescriptorType::SampledImage, lighting_output_[idx]},
        };
        UpdateDesc(device_, blit_ds_[idx], params, 1);

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

    // Pass 6: Geometry line overlay
    if (!geometry_entity_ids_.empty()) {
        line_renderer_.BeginFrame();
        for (auto eid : geometry_entity_ids_) {
            auto geom = geometry::component::get(game_entity::entity_id{eid});
            if (!geom.is_valid()) continue;
            const auto& pts = geom.tessellate();
            if (pts.size() < 2) continue;
            line_renderer_.AddLines(pts.data(), static_cast<u32>(pts.size()));
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
