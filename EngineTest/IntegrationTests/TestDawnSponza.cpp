#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "TestDawnSponza.h"
#include "Engine/Graphics/Lumen/DDGIFitGrid.h"
#include "DawnSponzaTextures.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Common/CoreLogStream.h"
#include <cstdlib>
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <limits>
#include <iomanip>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#endif

using namespace primal;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::handles;

namespace rhimath = primal::graphics::rhi::math;

static void CaptureRequire(bool ok, const char* message) {
    if (!ok) {
        PRIMAL_LOG_STREAM(Error, "DawnSponza") << message << std::endl;
        std::_Exit(7);
    }
}

// ============================================================
// Construction / Destruction
// ============================================================

Engine_Test::Engine_Test() = default;
Engine_Test::~Engine_Test() = default;

// ============================================================
// Shader loading
// ============================================================

static std::string LoadShaderSource(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::ifstream file2("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + path);
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
// Texture helpers
// ============================================================

ResourceHandle Engine_Test::CreateTextureFromData(int w, int h, const unsigned char* data,
    DataFormat format) {
    return primal::tests::dawn_sponza::CreateTexture(*device_, w, h, data, format);
}

ResourceHandle Engine_Test::LoadTextureFromFile(const std::string& path, DataFormat format) {
    // Check cache first — include format in key so same path can have sRGB and linear versions
    std::string cacheKey = path + (format == DataFormat::RGBA8_sRGB ? "|srgb" : "|lin");
    auto it = textureCache_.find(cacheKey);
    if (it != textureCache_.end()) {
        return it->second;
    }

    static int loadCount = 0;
    loadCount++;
    int width, height, channels;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data) {
        std::cerr << "[DawnSponza] Failed to load texture: " << path << std::endl;
        return INVALID_RESOURCE;
    }

    ResourceHandle tex = CreateTextureFromData(width, height, data, format);
    stbi_image_free(data);

    std::cerr << "[TEX LOAD #" << (loadCount - 1) << "] " << width << "x" << height << " ch=" << channels
              << " handle=" << tex << " " << path << std::endl;

    if (tex != INVALID_RESOURCE) textureCache_[cacheKey] = tex;
    return tex;
}

// ============================================================
// Texture path resolution
// ============================================================

static std::string ResolveTexturePath(const std::string& basePath, const std::string& texPath) {
    if (texPath.empty()) return "";

    std::ifstream test(texPath);
    if (test.is_open()) return texPath;

    std::string fullPath = basePath + texPath;
    {
        std::ifstream t(fullPath);
        if (t.is_open()) return fullPath;
    }

    // Extract filename for subdirectory searches
    size_t lastSlash = texPath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) ? texPath.substr(lastSlash + 1) : texPath;

    // Try models/Sponza/
    {
        std::string path = basePath + "models/Sponza/" + filename;
        std::ifstream t(path);
        if (t.is_open()) return path;
    }

    // Try fbx_textures/
    {
        std::string path = basePath + "fbx_textures/" + filename;
        std::ifstream t(path);
        if (t.is_open()) return path;
    }

    // Try images/ (old format TGA files)
    {
        std::string path = basePath + "images/" + filename;
        std::ifstream t(path);
        if (t.is_open()) return path;
    }

    return "";
}

static std::string DeriveNormalPath(const std::string& diffusePath) {
    if (diffusePath.empty()) return "";
    // Try replacing common diffuse suffixes with normal suffixes
    std::string normal = diffusePath;
    if (normal.find("_diffuse.") != std::string::npos) {
        return normal.replace(normal.find("_diffuse."), 9, "_normal.");
    }
    if (normal.find("_Albedo.") != std::string::npos) {
        return normal.replace(normal.find("_Albedo."), 8, "_Normal.");
    }
    if (normal.find("_Diff.") != std::string::npos) {
        return normal.replace(normal.find("_Diff."), 6, "_Normal.");
    }
    return "";
}

static std::string DeriveRoughnessPath(const std::string& diffusePath) {
    if (diffusePath.empty()) return "";
    std::string rough = diffusePath;
    if (rough.find("_diffuse.") != std::string::npos) {
        return rough.replace(rough.find("_diffuse."), 9, "_roughness.");
    }
    if (rough.find("_Albedo.") != std::string::npos) {
        return rough.replace(rough.find("_Albedo."), 8, "_Roughness.");
    }
    return "";
}

static std::string DeriveMetallicPath(const std::string& diffusePath) {
    if (diffusePath.empty()) return "";
    std::string metal = diffusePath;
    if (metal.find("_diffuse.") != std::string::npos) {
        return metal.replace(metal.find("_diffuse."), 9, "_metallic.");
    }
    if (metal.find("_Albedo.") != std::string::npos) {
        return metal.replace(metal.find("_Albedo."), 8, "_Metallic.");
    }
    return "";
}

// ============================================================
// CreatePipeline
// ============================================================

bool Engine_Test::CreatePipelines() {
    std::string shaderSource = LoadShaderSource("EngineTest/shaders/DawnWGSL/ForwardPBR.wgsl");
    if (shaderSource.empty()) {
        std::cerr << "[DawnSponza] Failed to load ForwardPBR.wgsl" << std::endl;
        return false;
    }

    vs_ = device_->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Vertex, "forward_pbr_vs");
    if (!screenProbePlacementEnabled_)
        fs_ = device_->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, screenGatherEnabled_?"forward_gi_fs":"forward_pbr_fs");
    if (vs_ == INVALID_SHADER || (!screenProbePlacementEnabled_ && fs_ == INVALID_SHADER)) {
        std::cerr << "[DawnSponza] Failed to compile shaders" << std::endl;
        return false;
    }

    // Global descriptor set layout: ViewData uniform + LightData uniform
    {
        DescriptorSetLayoutBinding bindings[13]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = DescriptorType::UniformBuffer;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;

        bindings[1].binding = 5;
        bindings[1].descriptorType = DescriptorType::UniformBuffer;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = ShaderStage::Pixel;

        bindings[2] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
        bindings[3] = {2, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr};
        DescriptorSetLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        bindings[4] = {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
        bindings[5] = {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
        bindings[6] = {6, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
        bindings[7]={7,DescriptorType::SampledImage,1,ShaderStage::Pixel,nullptr};bindings[8]={8,DescriptorType::SampledImage,1,ShaderStage::Pixel,nullptr};bindings[9]={9,DescriptorType::UniformBuffer,1,ShaderStage::Pixel,nullptr};
        for(u32 i=10;i<13;++i)bindings[i]={i+2,DescriptorType::SampledImage,1,ShaderStage::Pixel,nullptr};
        layoutDesc.bindingCount = screenProbePlacementEnabled_?13:screenGatherEnabled_?6:4;
        globalLayout_ = device_->CreateDescriptorSetLayout(layoutDesc);
        if (globalLayout_ == INVALID_DESCRIPTOR_SET_LAYOUT) return false;
    }

    // Material descriptor set layout: diffuse + normal + ORM + sampler
    {
        DescriptorSetLayoutBinding bindings[4]{};
        bindings[0].binding = 1;
        bindings[0].descriptorType = DescriptorType::SampledImage;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = ShaderStage::Pixel;

        bindings[1].binding = 2;
        bindings[1].descriptorType = DescriptorType::SampledImage;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = ShaderStage::Pixel;

        bindings[2].binding = 3;
        bindings[2].descriptorType = DescriptorType::SampledImage;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = ShaderStage::Pixel;

        bindings[3].binding = 4;
        bindings[3].descriptorType = DescriptorType::Sampler;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = ShaderStage::Pixel;

        DescriptorSetLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = 4;
        materialLayout_ = device_->CreateDescriptorSetLayout(layoutDesc);
        if (materialLayout_ == INVALID_DESCRIPTOR_SET_LAYOUT) return false;
    }

    // Pipeline layout: group 0 = global, group 1 = material, push constants at group 2
    {
        DescriptorSetLayoutHandle setLayouts[] = {globalLayout_, materialLayout_};
        PushConstantRange pushRanges[1];
        pushRanges[0].stageFlags = ShaderStage::Vertex;
        pushRanges[0].offset = 0;
        pushRanges[0].size = 64; // mat4x4

        PipelineLayoutDesc layoutDesc{};
        layoutDesc.setLayouts = setLayouts;
        layoutDesc.setLayoutCount = 2;
        layoutDesc.pushConstantRanges = pushRanges;
        layoutDesc.pushConstantRangeCount = 1;
        pipelineLayout_ = device_->CreatePipelineLayout(layoutDesc);
        if (pipelineLayout_ == INVALID_PIPELINE_LAYOUT) return false;
    }

    // Graphics pipeline with vertex attributes (32-byte stride)
    {
        GraphicsPipelineDesc pipeDesc{};
        pipeDesc.vertexShader = vs_;
        pipeDesc.pixelShader = fs_;
        pipeDesc.layout = pipelineLayout_;
        pipeDesc.topology = PrimitiveTopology::TriangleList;
        pipeDesc.cullMode = CullMode::None;
        pipeDesc.renderTargetFormats[0] = DataFormat::RGBA16_Float;
        pipeDesc.renderTargetCount = 1;
        pipeDesc.depthStencilFormat = DataFormat::D32_Float;
        pipeDesc.enableDepthTest = true;
        pipeDesc.enableDepthWrite = true;

        // Vertex attributes matching 32-byte interleaved vertex format
        pipeDesc.vertexAttributes.push_back({0, 0, DataFormat::RGB32_Float, 0});   // position
        pipeDesc.vertexAttributes.push_back({1, 0, DataFormat::R32_UInt, 12});     // colorTSign
        pipeDesc.vertexAttributes.push_back({2, 0, DataFormat::R32_UInt, 16});     // packedNormal
        pipeDesc.vertexAttributes.push_back({3, 0, DataFormat::R32_UInt, 20});     // packedTangent
        pipeDesc.vertexAttributes.push_back({4, 0, DataFormat::RG32_Float, 24});   // uv

        pipeDesc.vertexBindings.push_back({0, 32, true}); // binding 0, stride 32, per-vertex

        normalShader_ = device_->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, screenProbePlacementEnabled_?"environment_prepass":screenGatherEnabled_?"gather_prepass":"normal_prepass");
        if (normalShader_ == INVALID_SHADER) return false;
        auto normalDesc = pipeDesc; normalDesc.pixelShader = normalShader_;
        // Frozen GPU-driven GBuffer quantization, shared by every consumer.
        normalDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        if(screenGatherEnabled_){normalDesc.renderTargetCount=screenProbePlacementEnabled_?3:2;normalDesc.renderTargetFormats[1]=DataFormat::BGRA8_UNorm;if(screenProbePlacementEnabled_)normalDesc.renderTargetFormats[2]=DataFormat::RGBA8_UNorm;}
        normalPipeline_ = device_->CreateGraphicsPipeline(normalDesc);
        if (normalPipeline_ == INVALID_PIPELINE) return false;
        if (!screenProbePlacementEnabled_) {
            pipeDesc.enableDepthWrite = false; pipeDesc.depthFunc = ComparisonFunc::LessEqual;
            pipeline_ = device_->CreateGraphicsPipeline(pipeDesc);
            if (pipeline_ == INVALID_PIPELINE) {
                std::cerr << "[DawnSponza] Failed to create graphics pipeline" << std::endl;
                return false;
            }
        }
    }

    // Match the frozen Sponza host sampler: trilinear repeat with 8x anisotropy.
    // Its explicit sampler overrides GPUDrivenDrawPipeline defaults; the same
    // sampler is also used for Surface Cache capture.
    {
        SamplerDesc samplerDesc{};
        samplerDesc.minFilter = FilterMode::Linear;
        samplerDesc.magFilter = FilterMode::Linear;
        samplerDesc.mipFilter = FilterMode::Linear;
        samplerDesc.addressU = TextureAddressMode::Wrap;
        samplerDesc.addressV = TextureAddressMode::Wrap;
        samplerDesc.addressW = TextureAddressMode::Wrap;
        samplerDesc.comparisonFunc = ComparisonFunc::Never;
        samplerDesc.maxAnisotropy = 8;
        samplerDesc.maxLod = 100.0f;
        defaultSampler_ = device_->CreateSampler(samplerDesc);
    }

    return true;
}

// ============================================================
// CreateResources
// ============================================================

bool Engine_Test::CreateResources() {
    TextureDesc hdrDesc{};
    hdrDesc.size = {width_, height_, 1}; hdrDesc.type = TextureType::Texture2D;
    hdrDesc.format = DataFormat::RGBA16_Float;
    hdrDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource;
    if (screenProbePlacementEnabled_) hdrDesc.usage = hdrDesc.usage | TextureUsage::UnorderedAccess;
    hdrTexture_ = device_->CreateTexture(hdrDesc);
    if(screenProbePlacementEnabled_){auto directDesc=hdrDesc;directDesc.usage=directDesc.usage|TextureUsage::UnorderedAccess;directTexture_=device_->CreateTexture(directDesc);if(directTexture_==INVALID_RESOURCE)return false;}
    if (screenProbePlacementEnabled_ && !fusion_.Initialize(*device_, width_, height_, hdrTexture_)) return false;
    if (hdrTexture_ == INVALID_RESOURCE || !postProcess_.Initialize(*device_, width_, height_)) return false;
    if(width_!=outputWidth_||height_!=outputHeight_){auto ldr=hdrDesc;ldr.format=DataFormat::BGRA8_UNorm;ldr.usage=TextureUsage::RenderTarget|TextureUsage::ShaderResource|TextureUsage::CopySource;ldrTexture_=device_->CreateTexture(ldr);if(ldrTexture_==INVALID_RESOURCE||!present_.Initialize(*device_,ldrTexture_,outputWidth_,outputHeight_))return false;}
    auto envFloat = [](const char* name, float fallback) {
        const char* value = std::getenv(name);
        const float parsed = value ? std::strtof(value, nullptr) : fallback;
        return std::isfinite(parsed) ? parsed : fallback;
    };
    toneParameters_.exposure = 0.1f * std::exp2(envFloat("PRIMAL_TONE_EV", 0.f));
    const char* adapt = std::getenv("PRIMAL_TONE_ADAPT");
    toneParameters_.autoAdapt = adapt && adapt[0] == '0' ? 0.f : 1.f;
    toneParameters_.key = envFloat("PRIMAL_TONE_KEY", 0.45f);
    toneParameters_.contrast = envFloat("PRIMAL_TONE_CONTRAST", 1.f);
    toneParameters_.saturation = envFloat("PRIMAL_TONE_SAT", 1.15f);
    toneParameters_.vignette = envFloat("PRIMAL_TONE_VIGNETTE", 0.45f);
    toneParameters_.bloomIntensity = envFloat("PRIMAL_BLOOM_INTENSITY", 4.f);
    PRIMAL_LOG_STREAM(Info, "DawnSponza") << "HDR postprocess: RGBA16F -> WGSL adaptation -> ACES/gamma BGRA8; exposure="
        << toneParameters_.exposure << " adapt=" << toneParameters_.autoAdapt << " key=" << toneParameters_.key
        << " contrast=" << toneParameters_.contrast << " saturation=" << toneParameters_.saturation
        << " vignette=" << toneParameters_.vignette << " bloom=unconnected" << std::endl;
    // Depth texture
    {
        TextureDesc desc{};
        desc.size = {width_, height_, 1};
        desc.format = DataFormat::D32_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource | TextureUsage::CopySource;
        depthTexture_ = device_->CreateTexture(desc);
        if (depthTexture_ == INVALID_RESOURCE) return false;
    }

    TextureDesc normalDesc{}; normalDesc.size = {width_,height_,1}; normalDesc.type = TextureType::Texture2D;
    normalDesc.format = DataFormat::BGRA8_UNorm; normalDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource;
    normalTexture_ = device_->CreateTexture(normalDesc);
    if (normalTexture_ == INVALID_RESOURCE || !shadows_.Initialize(*device_,width_,height_)) return false;

    if(screenGatherEnabled_){ormTexture_=device_->CreateTexture(normalDesc);if(ormTexture_==INVALID_RESOURCE)return false;}
    if(screenProbePlacementEnabled_){normalDesc.format=DataFormat::RGBA8_UNorm;albedoTexture_=device_->CreateTexture(normalDesc);if(albedoTexture_==INVALID_RESOURCE)return false;}

    if(screenProbePlacementEnabled_&&!deferredLighting_.Initialize(*device_,width_,height_,{depthTexture_,normalTexture_,ormTexture_,albedoTexture_,shadows_.Visibility(),directTexture_}))return false;

    // Per-frame uniform buffers and descriptor sets
    for (u32 i = 0; i < kFrameCount; ++i) {
        // ViewData buffer
        {
            BufferDesc desc{};
            desc.size = 256;
            desc.type = BufferType::Constant;
            desc.memoryUsage = GPUMemoryUsage::Dynamic;
            viewBuffers_[i] = device_->CreateBuffer(desc);
        }
        // LightData buffer
        {
            BufferDesc desc{};
            desc.size = 256;
            desc.type = BufferType::Constant;
            desc.memoryUsage = GPUMemoryUsage::Dynamic;
            lightBuffers_[i] = device_->CreateBuffer(desc);
        }
        // Global descriptor set
        {
            DescriptorSetDesc desc{};
            desc.layout = globalLayout_;
            globalSets_[i] = device_->CreateDescriptorSet(desc);
        }
        // Write descriptors: view buffer at binding 0, light buffer at binding 5
        {
            DescriptorBufferInfo viewBufInfo{viewBuffers_[i], 0, 256};
            DescriptorBufferInfo lightBufInfo{lightBuffers_[i], 0, 256};

            WriteDescriptorSet writes[4]{};
            writes[0].dstSet = globalSets_[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::UniformBuffer;
            writes[0].bufferInfo = &viewBufInfo;

            writes[1].dstSet = globalSets_[i];
            writes[1].dstBinding = 5;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::UniformBuffer;
            writes[1].bufferInfo = &lightBufInfo;

            DescriptorImageInfo shadowInfo{shadows_.Sampler(),shadows_.Visibility(),ResourceState::ShaderResource};
            writes[2].dstSet = globalSets_[i]; writes[2].dstBinding = 1; writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::SampledImage; writes[2].imageInfo = &shadowInfo;
            writes[3].dstSet = globalSets_[i]; writes[3].dstBinding = 2; writes[3].descriptorCount = 1;
            writes[3].descriptorType = DescriptorType::Sampler; writes[3].imageInfo = &shadowInfo;
            device_->UpdateDescriptorSets(4, writes);
            if(screenProbePlacementEnabled_){
                ResourceHandle textures[]{ormTexture_,albedoTexture_,directTexture_};DescriptorImageInfo images[3]{};WriteDescriptorSet extra[3]{};
                for(u32 j=0;j<3;++j){images[j]={handles::INVALID_SAMPLER,textures[j],ResourceState::ShaderResource};extra[j].dstSet=globalSets_[i];extra[j].dstBinding=12+j;extra[j].descriptorCount=1;extra[j].descriptorType=DescriptorType::SampledImage;extra[j].imageInfo=&images[j];}device_->UpdateDescriptorSets(3,extra);
            }
        }
    }

    return true;
}

// ============================================================
// LoadScene
// ============================================================

bool Engine_Test::LoadScene() {
    std::string baseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";

    const char* requestedModel = std::getenv("PRIMAL_TEST_MODEL");
    std::string modelPath = baseDir + (requestedModel ? requestedModel : "Sponza_process_rebuild.model");
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open() && !requestedModel) {
        modelPath = baseDir + "Sponza.model";
        file.open(modelPath, std::ios::binary | std::ios::ate);
    }
    if (!file.is_open()) {
        std::cerr << "[DawnSponza] Failed to open Sponza model file" << std::endl;
        return false;
    }
    PRIMAL_LOG_STREAM(Info, "DawnSponza") << "model=" << modelPath << std::endl;

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) {
        std::cerr << "[DawnSponza] Failed to read Sponza model" << std::endl;
        return false;
    }

    graphics::SceneDataAdapter adapter;
    sceneMeshes_ = adapter.LoadRenderItemData(device_, buffer.data(), (uint32_t)buffer.size());
    if (sceneMeshes_.empty()) {
        std::cerr << "[DawnSponza] Failed to parse Sponza model" << std::endl;
        return false;
    }

    std::cout << "[DawnSponza] Loaded " << sceneMeshes_.size() << " meshes" << std::endl;

    // Match RenderSceneSnapshot's SDF bounds first, raw-position fallback.
    shadowBounds_.clear();
    for (const auto& mesh : sceneMeshes_) {
        if (!mesh.mesh) continue;
        RHIMeshAsset asset{};
        if (!content::get_rhi_mesh_asset(content::get_rhi_mesh_id(mesh.meshEntityId), asset)) return false;
        float lo[3], hi[3]; bool valid = true;
        for (int axis=0; axis<3; ++axis) {
            lo[axis]=asset.sdf.bounds_min[axis]; hi[axis]=asset.sdf.bounds_max[axis];
            valid = valid && std::isfinite(lo[axis]) && std::isfinite(hi[axis]) &&
                std::abs(lo[axis])<=1e6f && std::abs(hi[axis])<=1e6f && hi[axis]-lo[axis]>=1e-6f;
        }
        if (!valid) {
            if (!asset.num_vertices || asset.position_buffer.size()<size_t(asset.num_vertices)*12) return false;
            for(int axis=0;axis<3;++axis){lo[axis]=std::numeric_limits<float>::max();hi[axis]=std::numeric_limits<float>::lowest();}
            const auto* positions=reinterpret_cast<const float*>(asset.position_buffer.data());
            for(u32 v=0;v<asset.num_vertices;++v) for(int axis=0;axis<3;++axis){
                const float value=positions[v*3+axis];if(!std::isfinite(value))return false;
                lo[axis]=std::min(lo[axis],value);hi[axis]=std::max(hi[axis],value);
            }
        }
        shadowBounds_.push_back({{(lo[0]+hi[0])*.5f,(lo[1]+hi[1])*.5f,(lo[2]+hi[2])*.5f},
            std::max({hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2]})*.5f});
    }
    if (shadowBounds_.empty()) return false;
    PRIMAL_LOG_STREAM(Info,"DawnSponza") << "Shadows: four 2048 CSM extents=12,30,90,270, PCSS, scene bounds=" << shadowBounds_.size() << std::endl;

    PRIMAL_LOG_STREAM(Info, "DawnSponza") << "Material inputs: albedo=RGBA8_sRGB, albedo mips=WGSL sRGB linear blit, normal/ORM mips=CPU area box" << std::endl;

    // Load textures and create material descriptor sets
    std::string textureBase = baseDir;
    meshMaterials_.resize(sceneMeshes_.size());

    u32 texLoadedDiffuse = 0, texLoadedNormal = 0, texLoadedORM = 0;
    u32 texFailedDiffuse = 0, texFailedNormal = 0;

    for (u32 i = 0; i < sceneMeshes_.size(); ++i) {
        const auto& meshInfo = sceneMeshes_[i];
        auto& mat = meshMaterials_[i];

        // Diffuse texture (fallback: white 1x1)
        std::string diffusePath = ResolveTexturePath(textureBase, meshInfo.diffuseTexturePath);
        if (!diffusePath.empty()) {
            mat.diffuseTex = LoadTextureFromFile(diffusePath, DataFormat::RGBA8_sRGB);
        }
        // Print texture diagnostic for key meshes only
        {
            bool isKey = (i < 10) || (meshInfo.materialIndex >= 12 && meshInfo.materialIndex <= 21);
            if (isKey) {
                std::cerr << "[MESH TEX] mesh=" << i << " matIdx=" << meshInfo.materialIndex
                          << " diffRaw='" << meshInfo.diffuseTexturePath
                          << "' diffResolved='" << diffusePath << "'"
                          << " diffHandle=" << mat.diffuseTex
                          << std::endl;
            }
        }
        if (mat.diffuseTex == INVALID_RESOURCE) {
            std::cerr << "[DIFFUSE FAIL] mesh=" << i << " matIdx=" << meshInfo.materialIndex
                      << " path='" << meshInfo.diffuseTexturePath << "'" << std::endl;
            unsigned char white[4] = {255, 255, 255, 255};
            mat.diffuseTex = CreateTextureFromData(1, 1, white, DataFormat::RGBA8_sRGB);
            ++texFailedDiffuse;
        } else {
            ++texLoadedDiffuse;
        }

        // Normal texture - try model path first, then derive from diffuse
        std::string normalPath = ResolveTexturePath(textureBase, meshInfo.normalTexturePath);
        if (normalPath.empty() && !meshInfo.diffuseTexturePath.empty()) {
            std::string derived = DeriveNormalPath(meshInfo.diffuseTexturePath);
            normalPath = ResolveTexturePath(textureBase, derived);
        }
        if (!normalPath.empty()) {
            mat.normalTex = LoadTextureFromFile(normalPath);
        }
        if (mat.normalTex == INVALID_RESOURCE) {
            unsigned char flatNormal[4] = {128, 128, 255, 255};
            mat.normalTex = CreateTextureFromData(1, 1, flatNormal);
            ++texFailedNormal;
        } else {
            ++texLoadedNormal;
        }

        // ORM texture - try to construct from roughness + metallic maps
        {
            std::string roughnessPath = ResolveTexturePath(textureBase, meshInfo.roughnessTexturePath);
            if (roughnessPath.empty() && !meshInfo.diffuseTexturePath.empty()) {
                roughnessPath = ResolveTexturePath(textureBase, DeriveRoughnessPath(meshInfo.diffuseTexturePath));
            }
            std::string metallicPath = ResolveTexturePath(textureBase, meshInfo.metallicTexturePath);
            if (metallicPath.empty() && !meshInfo.diffuseTexturePath.empty()) {
                metallicPath = ResolveTexturePath(textureBase, DeriveMetallicPath(meshInfo.diffuseTexturePath));
            }

            // Try ORM combined texture first
            std::string ormPath = ResolveTexturePath(textureBase, meshInfo.ormTexturePath);
            if (!ormPath.empty()) {
                mat.ormTex = LoadTextureFromFile(ormPath);
            }

            // If no ORM, try to construct from separate roughness + metallic textures
            if (mat.ormTex == INVALID_RESOURCE && !roughnessPath.empty()) {
                int rw, rh, rc;
                unsigned char* roughData = stbi_load(roughnessPath.c_str(), &rw, &rh, &rc, 4);
                int mw = 0, mh = 0;
                unsigned char* metalData = nullptr;
                if (!metallicPath.empty()) {
                    metalData = stbi_load(metallicPath.c_str(), &mw, &mh, &rc, 4);
                }

                if (roughData && rw > 0 && rh > 0) {
                    // Construct ORM: R=AO(255), G=Roughness, B=Metallic, A=255
                    std::vector<unsigned char> ormData(rw * rh * 4);
                    for (int p = 0; p < rw * rh; ++p) {
                        ormData[p * 4 + 0] = 255; // AO = 1.0
                        ormData[p * 4 + 1] = roughData[p * 4]; // Roughness from green
                        ormData[p * 4 + 2] = (metalData && mw == rw && mh == rh) ? metalData[p * 4] : 0;
                        ormData[p * 4 + 3] = 255;
                    }
                    mat.ormTex = CreateTextureFromData(rw, rh, ormData.data());
                    ++texLoadedORM;
                }

                if (roughData) stbi_image_free(roughData);
                if (metalData) stbi_image_free(metalData);
            }

            if (mat.ormTex == INVALID_RESOURCE) {
                unsigned char defaultORM[4] = {255, 128, 0, 255};
                mat.ormTex = CreateTextureFromData(1, 1, defaultORM);
            }
        }

        // Material descriptor set
        {
            DescriptorSetDesc desc{};
            desc.layout = materialLayout_;
            mat.materialSet = device_->CreateDescriptorSet(desc);
        }
        // Write material descriptors: 3 textures + 1 sampler
        {
            DescriptorImageInfo diffuseImg{INVALID_SAMPLER, mat.diffuseTex, ResourceState::ShaderResource};
            DescriptorImageInfo normalImg{INVALID_SAMPLER, mat.normalTex, ResourceState::ShaderResource};
            DescriptorImageInfo ormImg{INVALID_SAMPLER, mat.ormTex, ResourceState::ShaderResource};
            DescriptorImageInfo sampImg{defaultSampler_, INVALID_RESOURCE, ResourceState::Unknown};

            WriteDescriptorSet writes[4]{};
            writes[0].dstSet = mat.materialSet;
            writes[0].dstBinding = 1;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::SampledImage;
            writes[0].imageInfo = &diffuseImg;

            writes[1].dstSet = mat.materialSet;
            writes[1].dstBinding = 2;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::SampledImage;
            writes[1].imageInfo = &normalImg;

            writes[2].dstSet = mat.materialSet;
            writes[2].dstBinding = 3;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::SampledImage;
            writes[2].imageInfo = &ormImg;

            writes[3].dstSet = mat.materialSet;
            writes[3].dstBinding = 4;
            writes[3].descriptorCount = 1;
            writes[3].descriptorType = DescriptorType::Sampler;
            writes[3].imageInfo = &sampImg;

            device_->UpdateDescriptorSets(4, writes);
        }
    }

    std::cerr << "[DawnSponza] Texture loading: diffuse=" << texLoadedDiffuse << "/" << (texLoadedDiffuse + texFailedDiffuse)
              << " normal=" << texLoadedNormal << "/" << (texLoadedNormal + texFailedNormal)
              << " orm=" << texLoadedORM << " (constructed from roughness/metallic)" << std::endl;

    // Verify bind groups can be built for all meshes
    {
        u32 bindGroupOK = 0, bindGroupFailed = 0;
        for (u32 i = 0; i < sceneMeshes_.size(); ++i) {
            auto* ds = device_->GetDescriptorSet(meshMaterials_[i].materialSet);
            if (!ds) {
                std::cerr << "[BIND GROUP] mesh=" << i << " matIdx=" << sceneMeshes_[i].materialIndex
                          << " FAILED: null descriptor set" << std::endl;
                ++bindGroupFailed;
                continue;
            }
            WGPUBindGroup bg = ds->GetBindGroup();
            if (!bg) {
                std::cerr << "[BIND GROUP] mesh=" << i << " matIdx=" << sceneMeshes_[i].materialIndex
                          << " FAILED: null bind group (diff=" << meshMaterials_[i].diffuseTex
                          << " norm=" << meshMaterials_[i].normalTex
                          << " orm=" << meshMaterials_[i].ormTex << ")" << std::endl;
                ++bindGroupFailed;
            } else {
                ++bindGroupOK;
            }
        }
        std::cerr << "[DawnSponza] Bind groups: " << bindGroupOK << " OK, " << bindGroupFailed << " FAILED" << std::endl;
    }

    return true;
}

// ============================================================
// Camera update
// ============================================================

void Engine_Test::UpdateCamera(float dt) {
    float speed = cameraSpeed_ * dt;

#ifdef __APPLE__
    auto keyPressed = [](uint16_t keyCode) -> bool {
        return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, keyCode);
    };
    // WASD: W=13, A=0, S=1, D=2
    if (keyPressed(13)) { // W - forward
        cameraPos_.x += sinf(cameraYaw_) * speed;
        cameraPos_.z += cosf(cameraYaw_) * speed;
    }
    if (keyPressed(1)) { // S - backward
        cameraPos_.x -= sinf(cameraYaw_) * speed;
        cameraPos_.z -= cosf(cameraYaw_) * speed;
    }
    if (keyPressed(0)) { // A - left
        cameraPos_.x += cosf(cameraYaw_) * speed;
        cameraPos_.z -= sinf(cameraYaw_) * speed;
    }
    if (keyPressed(2)) { // D - right
        cameraPos_.x -= cosf(cameraYaw_) * speed;
        cameraPos_.z += sinf(cameraYaw_) * speed;
    }
    // Q/E for up/down: Q=12, E=14
    if (keyPressed(12)) cameraPos_.y -= speed;
    if (keyPressed(14)) cameraPos_.y += speed;
#endif
}

// ============================================================
// Initialize
// ============================================================

bool Engine_Test::initialize() {
    // Fixed reference profile keeps the frozen 160x90 probe lattice at native
    // Vulkan internal image resolution. FinalBlit still outputs 1280x720.
    if(const char* reference=std::getenv("PRIMAL_DAWN_REFERENCE_RESOLUTION");reference&&std::strcmp(reference,"1")==0){width_=2560;height_=1440;probeSpacing_=16;}
    std::cout << "[DawnSponza] Initializing..." << std::endl;

    // 1. Create platform window
    platform::window_init_info windowInfo{
        nullptr, nullptr,
        "Dawn WebGPU Sponza",
        100, 100, (s32)outputWidth_, (s32)outputHeight_
    };
    window_ = platform::create_window(&windowInfo);
    if (!window_.is_valid()) {
        std::cerr << "[DawnSponza] Failed to create window" << std::endl;
        return false;
    }

    // 2. Create Dawn device
    DeviceDesc deviceDesc{};
    deviceDesc.platform = RHIPlatform::Dawn;
    deviceDesc.enableDebug = true;
    deviceDesc.enableValidation = false;
    deviceDesc.maxFramesInFlight = kFrameCount;

    device_ = new DawnDevice(deviceDesc);
    if (!device_->Initialize()) {
        std::cerr << "[DawnSponza] Failed to initialize Dawn device" << std::endl;
        return false;
    }

    // 3. Create swapchain
    SwapChainDesc scDesc{};
    scDesc.window = window_.handle();
    scDesc.width = outputWidth_;
    scDesc.height = outputHeight_;
    scDesc.format = DataFormat::BGRA8_UNorm;
    scDesc.bufferCount = kFrameCount;

    swapchain_ = device_->CreateSwapChain(scDesc);
    if (!swapchain_) {
        std::cerr << "[DawnSponza] Failed to create swapchain" << std::endl;
        return false;
    }

    const char* gatherMode=std::getenv("PRIMAL_DAWN_SCREEN_GATHER");
    const char* spgiStage=std::getenv("PRIMAL_DAWN_SCREEN_PROBES_STAGE1");
    screenProbePlacementEnabled_=spgiStage&&std::strcmp(spgiStage,"1")==0;
    taaEnabled_=screenProbePlacementEnabled_&&std::getenv("TAA_OFF")==nullptr;
    screenGatherEnabled_=screenProbePlacementEnabled_||(gatherMode&&std::strcmp(gatherMode,"1")==0);
    // 4. Create pipelines and shaders
    if (!CreatePipelines()) {
        std::cerr << "[DawnSponza] Failed to create pipelines" << std::endl;
        return false;
    }

    // 5. Create GPU resources
    if (!CreateResources()) {
        std::cerr << "[DawnSponza] Failed to create resources" << std::endl;
        return false;
    }

    // 6. Load Sponza scene
    if (!LoadScene()) {
        std::cerr << "[DawnSponza] Failed to load scene" << std::endl;
        return false;
    }

    const char* tileMode=std::getenv("PRIMAL_DAWN_TILE_RADIOSITY");
    tileRadiosityEnabled_=screenGatherEnabled_ || (tileMode && std::strcmp(tileMode,"1")==0);
    const char* probeMode=std::getenv("PRIMAL_DAWN_PROBE_UPDATE");
    probeUpdateEnabled_=tileRadiosityEnabled_ || (probeMode && std::strcmp(probeMode,"1")==0);
    const char* radianceMode=std::getenv("PRIMAL_DAWN_MEASURED_RADIANCE");
    measuredRadianceEnabled_=probeUpdateEnabled_ || (radianceMode && std::strcmp(radianceMode,"1")==0);
    const char* lightingMode=std::getenv("PRIMAL_DAWN_SURFACE_LIGHTING");
    surfaceLightingEnabled_=measuredRadianceEnabled_ || (lightingMode && std::strcmp(lightingMode,"1")==0);
    const char* geometryMode=std::getenv("PRIMAL_DAWN_MEASURED_GEOMETRY");
    measuredGeometryEnabled_=surfaceLightingEnabled_ || (geometryMode && std::strcmp(geometryMode,"1")==0);
    if(measuredGeometryEnabled_){
        std::vector<graphics::ray_geometry::InstanceInput> instances;
        for(u32 i=0;i<sceneMeshes_.size();++i){
            const auto& mesh=sceneMeshes_[i];if(!mesh.mesh)return false;
            auto blas=content::get_rhi_mesh_blas(content::get_rhi_mesh_id(mesh.meshEntityId));
            if(!blas)return false;
            instances.push_back({blas,rhimath::MatrixIdentity(),i});
        }
        const auto grid=graphics::lumen::DDGIFitGridFromBounds({-19.21f,-1.26f,-11.83f},{18.00f,14.29f,11.05f},3.f);
        std::vector<std::array<float,4>> origins;
        for(u32 z=0;z<grid.count_z;++z)for(u32 y=0;y<grid.count_y;++y)for(u32 x=0;x<grid.count_x;++x)
            origins.push_back({grid.origin.x+x*3.f,grid.origin.y+y*3.f,grid.origin.z+z*3.f,1.f});
        const char* relocationCapture=std::getenv("SPONZA_RELOCATION_CAPTURE_PREFIX");
        if(!probeRelocation_.Load(*device_,"EngineTest/assets/Sponza_global_sdf.v3.cache",origins,3.f,relocationCapture))return false;
        probeCapacity_=probeUpdateEnabled_?u32(origins.size()):220u;
        if(!measuredGeometry_.Initialize(*device_,instances,origins,probeCapacity_,128,50.f,&probeRelocation_))return false;
        if(probeUpdateEnabled_){
            primal::tests::dawn_sponza::ProbeSchedule::Grid scheduleGrid{{grid.count_x,grid.count_y,grid.count_z},{grid.origin.x,grid.origin.y,grid.origin.z},3.f};
            if(!probeSchedule_.Initialize(scheduleGrid)||!probeDepthHistory_.Initialize(*device_,probeCapacity_,measuredGeometry_.SceneDigest(),probeRelocation_.Generation()))return false;
        }
        if(const char* path=std::getenv("SPONZA_GEOMETRY_SCENE_CAPTURE"))
            if(!measuredGeometry_.SaveSceneFixture(path))return false;
    }

    const char* scMode=std::getenv("PRIMAL_DAWN_SURFACE_CACHE");
    surfaceCacheEnabled_=surfaceLightingEnabled_ || (scMode && std::strcmp(scMode,"1")==0);
    if(surfaceCacheEnabled_){
        std::vector<ResourceHandle> albedos;for(const auto& material:meshMaterials_)albedos.push_back(material.diffuseTex);
        if(!surfaceCache_.Initialize(*device_,std::span(sceneMeshes_.data(),sceneMeshes_.size()),albedos,defaultSampler_,std::getenv("SPONZA_SURFACE_CAPTURE_PREFIX")))return false;
    }

    if(tileRadiosityEnabled_){
        primal::tests::dawn_sponza::TileRadiosity::Inputs in{};in.cards=surfaceCache_.Cards();in.cardData=surfaceCache_.CardData();in.lookups=surfaceCache_.Directory();in.lookupCount=surfaceCache_.OwnerCount();
        for(u32 i=0;i<3;++i)in.material[i]=surfaceCache_.Atlas(i);in.sdf=probeRelocation_.Field();const auto& p=probeRelocation_.Parameters();
        std::copy_n(p.origin,3,in.params.origin);in.params.origin[3]=float(p.counts[0]);std::copy_n(p.extent,3,in.params.extent);
        if(!tileRadiosity_.Initialize(*device_,in))return false;
    }
    if(surfaceLightingEnabled_){
        primal::tests::dawn_sponza::SurfaceCacheLighting::Inputs in{};
        in.cards=surfaceCache_.Cards();in.cardData=surfaceCache_.CardData();
        for(u32 i=0;i<3;++i)in.material[i]=surfaceCache_.Atlas(i);
        in.sdf=probeRelocation_.Field();const auto& p=probeRelocation_.Parameters();
        std::copy_n(p.origin,3,in.shadow.origin);in.shadow.origin[3]=float(p.counts[0]);
        std::copy_n(p.extent,3,in.shadow.extent);in.shadow.counts[3]=1;
        const float length=std::sqrt(.5f*.5f+.7f*.7f+.3f*.3f);
        in.shadow.toward[0]=-.5f/length;in.shadow.toward[1]=.7f/length;in.shadow.toward[2]=-.3f/length;
        for(u32 i=0;i<4;++i)in.csm[i]=shadows_.Map(i);
        if(!surfaceLighting_.Initialize(*device_,in))return false;
    }

    if(measuredRadianceEnabled_){
        primal::tests::dawn_sponza::MeasuredRadiance::Inputs in{};
        in.cards=surfaceCache_.Cards();in.directory=surfaceCache_.Directory();in.depth=surfaceCache_.Atlas(2);
        in.rayCount=measuredGeometry_.RayCount();in.cardCount=u32(surfaceCache_.CardData().size());in.ownerCount=surfaceCache_.OwnerCount();in.sceneDigest=measuredGeometry_.SceneDigest();
        if(!measuredRadiance_.Initialize(*device_,in))return false;
    }

    if(probeUpdateEnabled_){
        primal::tests::dawn_sponza::ProbeUpdate::Config c{};c.probes=u32(probeRelocation_.Points().size());c.budget=probeCapacity_;c.sceneDigest=measuredGeometry_.SceneDigest();c.voxel=probeRelocation_.Parameters().policy[1];
        if(!probeUpdate_.Initialize(*device_,c))return false;
    }
    if(screenGatherEnabled_){
        primal::tests::dawn_sponza::ScreenGather::Inputs in{};in.scene=measuredGeometry_.GetSceneView();
        in.depth=depthTexture_;in.normal=normalTexture_;in.orm=ormTexture_;in.width=width_;in.height=height_;
        // Frozen Sponza: logical viewport / base divisor 2, then half-size gather.
        // Raster supersampling must not increase this independent GI lattice.
        in.targetWidth=std::max(1u,outputWidth_/4);in.targetHeight=std::max(1u,outputHeight_/4);
        if(!screenGather_.Initialize(*device_,in))return false;
        for(auto set:globalSets_){DescriptorImageInfo images[2]{};images[0]={handles::INVALID_SAMPLER,screenGather_.Diffuse(),ResourceState::ShaderResource};images[1]={handles::INVALID_SAMPLER,screenGather_.Specular(),ResourceState::ShaderResource};WriteDescriptorSet writes[2]{};
            for(u32 i=0;i<2;++i){writes[i].dstSet=set;writes[i].dstBinding=3+i;writes[i].descriptorCount=1;writes[i].descriptorType=DescriptorType::SampledImage;writes[i].imageInfo=&images[i];}device_->UpdateDescriptorSets(2,writes);}
        PRIMAL_LOG_STREAM(Info,"DawnSponza") << "Native mode7 screen DDGI enabled";
    }
    if(screenProbePlacementEnabled_){
        if(!screenProbes_.Initialize(*device_,width_,height_,probeSpacing_,16,depthTexture_,normalTexture_))return false;
        if(!screenProbeHZB_.Initialize(*device_,width_,height_,depthTexture_,screenProbes_.ProbeCount(),16))return false;
        const std::array<u32,4> auditRays{5981,11104,134273,51552};
        const bool audit=std::getenv("PRIMAL_DAWN_SPGI_SDF_AUDIT")!=nullptr;
        if(!screenProbeSDF_.Initialize(*device_,probeRelocation_.Field(),probeRelocation_.Parameters(),screenProbes_.ProbeCount(),16,audit?std::span<const u32>(auditRays):std::span<const u32>{}))return false;
        primal::tests::dawn_sponza::ScreenProbeFinalize::Inputs in{};
        in.cards=surfaceCache_.Cards();in.grid=surfaceCache_.GridOffsets();in.gridCards=surfaceCache_.GridCards();in.depth=surfaceCache_.Atlas(2);in.albedo=surfaceCache_.Atlas(0);
        in.probes=screenProbes_.ProbeCount();in.rays=16;in.cardsCount=u32(surfaceCache_.CardData().size());in.worldProbes=u32(probeRelocation_.Points().size());in.sceneDigest=measuredGeometry_.SceneDigest();
        if(!screenProbeFinalize_.Initialize(*device_,in))return false;
        if(!screenProbeAverage_.Initialize(*device_,in.probes,in.rays))return false;
        if(!screenProbeHistory_.Initialize(*device_,(width_+probeSpacing_-1)/probeSpacing_,(height_+probeSpacing_-1)/probeSpacing_,measuredGeometry_.GetSceneView()))return false;
        if(!screenProbeGather_.Initialize(*device_,width_,height_,depthTexture_,normalTexture_))return false;
        for(auto set:globalSets_){DescriptorImageInfo image{handles::INVALID_SAMPLER,screenProbeGather_.Filtered(),ResourceState::ShaderResource};WriteDescriptorSet write{};write.dstSet=set;write.dstBinding=6;write.descriptorCount=1;write.descriptorType=DescriptorType::SampledImage;write.imageInfo=&image;device_->UpdateDescriptorSets(1,&write);}
    }
    if(taaEnabled_&&!temporalAA_.Initialize(*device_,width_,height_,depthTexture_))return false;
    if(screenProbePlacementEnabled_){
        primal::tests::dawn_sponza::EnvironmentVisibility::Inputs in{};in.scene=measuredGeometry_.GetSceneView();in.depth=depthTexture_;in.normal=normalTexture_;in.orm=ormTexture_;in.width=width_;in.height=height_;in.targetWidth=(outputWidth_+1)/2;in.targetHeight=(outputHeight_+1)/2;
        if(!environmentVisibility_.Initialize(*device_,in))return false;
        int hdrW=0,hdrH=0,hdrChannels=0;stbi_set_flip_vertically_on_load(false);auto* hdr=stbi_loadf("EngineTest/assets/textures/hdr/sunset.hdr",&hdrW,&hdrH,&hdrChannels,4);if(!hdr||hdrW<=0||hdrH<=0)return false;
        std::vector<u16> half(u64(hdrW)*hdrH*4);for(size_t i=0;i<half.size();++i)half[i]=primal::tests::dawn_sponza::EnvironmentIBL::FrozenHalf(hdr[i]);stbi_image_free(hdr);
        if(!environmentIBL_.Initialize(*device_,hdrW,hdrH,half,{}))return false;
        if(const char* path=std::getenv("SPONZA_IBL_CAPTURE"))if(!environmentIBL_.Save(path))return false;
        if(!environmentColor_.Initialize(*device_,width_,height_,{depthTexture_,normalTexture_,ormTexture_,albedoTexture_,environmentIBL_.Prefilter(),environmentIBL_.BRDF()}))return false;
        BufferDesc cb{};cb.type=BufferType::Constant;cb.size=16;cb.memoryUsage=GPUMemoryUsage::Static;environmentPolicy_=device_->CreateBuffer(cb);if(environmentPolicy_==INVALID_RESOURCE)return false;
        float displayScale=.5f;if(const char* e=std::getenv("PRIMAL_IBL_SPECULAR_SCALE")){const float v=std::strtof(e,nullptr);if(std::isfinite(v))displayScale=std::max(0.f,v);}const float policy[4]{1,displayScale,0,1};if(!device_->UpdateBufferData(environmentPolicy_,policy,16))return false;
        std::copy(policy, policy+4, fusionParameters_.environment);
        if (const char* e=std::getenv("PRIMAL_GI_INDIRECT_GAIN")) { const float v=std::strtof(e,nullptr); if(std::isfinite(v))fusionParameters_.gainMode[0]=std::clamp(v,0.f,8.f); }
        for(auto set:globalSets_){DescriptorImageInfo images[2]{{handles::INVALID_SAMPLER,environmentColor_.Output(),ResourceState::ShaderResource},{handles::INVALID_SAMPLER,environmentColor_.Output(),ResourceState::ShaderResource}};DescriptorBufferInfo bi{environmentPolicy_,0,16};WriteDescriptorSet writes[3]{};for(u32 i=0;i<3;++i){writes[i].dstSet=set;writes[i].dstBinding=7+i;writes[i].descriptorCount=1;writes[i].descriptorType=i==2?DescriptorType::UniformBuffer:DescriptorType::SampledImage;if(i==2)writes[i].bufferInfo=&bi;else writes[i].imageInfo=&images[i];}device_->UpdateDescriptorSets(3,writes);}
        PRIMAL_LOG_STREAM(Info,"DawnSponza") << "Native environment color/visibility/composition enabled; HDR=" << hdrW << "x" << hdrH << " prefilter_mips=" << environmentIBL_.Mips() << " scale=" << displayScale;
    }
    std::cerr << "[DawnSponza] Initialized. Rendering Sponza at " << width_ << "x" << height_ << std::endl;
    return true;
}

// ============================================================
// Run – one frame
// ============================================================

void Engine_Test::run() {
    if (!device_ || !swapchain_ || shuttingDown_) return;

    timer_.begin();

    float dt = 1.0f / 60.0f;
    if (!std::getenv("SPGI_CAM_LOCK")) UpdateCamera(dt);

    // Build view-projection matrix using RHIMath utilities
    float aspectRatio = (float)width_ / (float)height_;

    float cosY = cosf(cameraYaw_), sinY = sinf(cameraYaw_);
    float cosP = cosf(cameraPitch_), sinP = sinf(cameraPitch_);
    rhimath::v3 forward = rhimath::Normalize(rhimath::v3{sinY * cosP, sinP, cosY * cosP});
    rhimath::v3 target = cameraPos_ + forward;

    rhimath::m4x4 viewMatrix = rhimath::CreateLookAtMatrix(cameraPos_, target, rhimath::v3{0.0f, 1.0f, 0.0f});
    // Same RH [0,1] projection as Vulkan/Metal. The legacy MatrixPerspective
    // helper has m[3][3]=1, which shifts clip.w and changes scene framing.
    rhimath::m4x4 projMatrix = rhimath::CreatePerspectiveMatrix(60.0f * rhimath::constants::DEG_TO_RAD, aspectRatio, 0.1f, 1000.0f);
    rhimath::m4x4 viewProj = projMatrix * viewMatrix;
    const auto rasterJitter=taaEnabled_?graphics::utils::GetJitterOffset(frameIndex_,width_,height_):primal::math::v2{0,0};
    primal::tests::dawn_sponza::TemporalAA::Raster raster{};raster.frame=frameIndex_;raster.width=width_;raster.height=height_;raster.depth=depthTexture_;raster.baseVP=viewProj;raster.jitter=rasterJitter;

    // Update per-frame uniform buffers
    u32 frameIdx = frameIndex_ % kFrameCount;
    {
        struct ViewDataCPU {
            rhimath::m4x4 viewMatrix;
            rhimath::m4x4 projectionMatrix;
            rhimath::m4x4 invViewProjection;
            rhimath::v4 cameraPos;
            rhimath::v4 rasterJitter;
        };
        static_assert(sizeof(ViewDataCPU)==224);
        ViewDataCPU vd{};
        vd.viewMatrix = viewMatrix;
        vd.projectionMatrix = projMatrix;
        vd.invViewProjection = rhimath::Inverse(viewProj);
        vd.cameraPos = rhimath::v4{cameraPos_.x, cameraPos_.y, cameraPos_.z, 0.0f};
        vd.rasterJitter={rasterJitter.x,rasterJitter.y,0,0};
        CaptureRequire(device_->UpdateBufferData(viewBuffers_[frameIdx],&vd,sizeof(vd)),"actual raster matrix upload failed");
        raster.uploaded=true;
    }
    {
        struct LightDataCPU {
            rhimath::v4 direction;
            rhimath::v4 color;
        };
        LightDataCPU ld{};
        ld.direction = rhimath::v4{0.5f, -0.7f, 0.3f, 3.0f};
        ld.color = rhimath::v4{1.0f, 1.0f, 1.0f, 0.0f};
        void* mapped = device_->MapBuffer(lightBuffers_[frameIdx], 0, sizeof(LightDataCPU));
        if (mapped) {
            memcpy(mapped, &ld, sizeof(LightDataCPU));
            device_->UnmapBuffer(lightBuffers_[frameIdx]);
        }
    }

    device_->BeginFrame();

    u32 imageIndex = 0;
    if (!swapchain_->AcquireNextImage(&imageIndex)) {
        device_->EndFrame();
        frameIndex_++;
        return;
    }

    ResourceHandle backBuffer = swapchain_->GetBackBuffer(imageIndex);
    const char* capturePath = std::getenv("SPONZA_CAPTURE");
    const char* captureFrame = std::getenv("SPONZA_CAPTURE_FRAME");
    const bool capture = capturePath && frameIndex_ + 1 == u32(captureFrame ? std::atoi(captureFrame) : 350);
    ResourceHandle captureTexture = INVALID_RESOURCE, readback = INVALID_RESOURCE, adaptReadback = INVALID_RESOURCE, ldrReadback = INVALID_RESOURCE;
    const char* ldrCapturePath=std::getenv("SPONZA_LDR_CAPTURE");
    const u32 ldrPitch=(width_*4u+255u)&~255u;
    const u32 rowBytes = (outputWidth_ * 4u + 255u) & ~255u;
    if (capture) {
        TextureDesc td{}; td.size = {outputWidth_, outputHeight_, 1}; td.format = DataFormat::BGRA8_UNorm;
        td.type = TextureType::Texture2D;
        td.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
        captureTexture = device_->CreateTexture(td);
        BufferDesc bd{}; bd.size = u64(rowBytes) * outputHeight_; bd.memoryUsage = GPUMemoryUsage::Readback;
        readback = device_->CreateBuffer(bd);
        bd.size = 4; adaptReadback = device_->CreateBuffer(bd);
        CaptureRequire(captureTexture != INVALID_RESOURCE && readback != INVALID_RESOURCE && adaptReadback != INVALID_RESOURCE, "capture allocation failed");
        backBuffer = captureTexture;
        if(ldrCapturePath&&ldrTexture_!=INVALID_RESOURCE){bd.size=u64(ldrPitch)*height_;ldrReadback=device_->CreateBuffer(bd);CaptureRequire(ldrReadback!=INVALID_RESOURCE,"LDR capture allocation failed");}
    }

    primal::tests::dawn_sponza::ProbeSchedule::Plan probePlan;
    if(probeUpdateEnabled_){
        probeGovernor_.RecordFrame();
        primal::tests::dawn_sponza::ProbeSchedule::DepthHistory depthHistory;
        CaptureRequire(probeDepthHistory_.Consume(frameIndex_,depthHistory),"N-2 probe depth readback failed");
        const u32 budget=graphics::GIAdaptiveGovernor::ScaleBudget(probeCapacity_,probeGovernor_.Scale());
        CaptureRequire(probeSchedule_.Prepare(frameIndex_,frameIndex_,frameIndex_,budget,graphics::lumen::GIWarmupFrames(),
            {cameraPos_.x,cameraPos_.y,cameraPos_.z},depthHistory,probePlan,std::getenv("DDGI_AGE_CAP")!=nullptr),"probe schedule preparation failed");
        if(const char* prefix=std::getenv("SPONZA_PROBE_SCHEDULE_CAPTURE_PREFIX")){
            if(frameIndex_==2 || frameIndex_==3 || frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350)){
                const std::string base=std::string(prefix)+"-"+std::to_string(frameIndex_);
                std::ofstream raw(base+".bin",std::ios::binary),meta(base+".json");
                const auto ages=probeSchedule_.LastUpdates();raw.write(reinterpret_cast<const char*>(ages.data()),ages.size_bytes());
                if(!depthHistory.values.empty())raw.write(reinterpret_cast<const char*>(depthHistory.values.data()),depthHistory.values.size_bytes());
                raw.write(reinterpret_cast<const char*>(probePlan.indices.data()),probePlan.indices.size()*4);
                raw.write(reinterpret_cast<const char*>(probePlan.scores.data()),probePlan.scores.size()*4);
                const auto grid=graphics::lumen::DDGIFitGridFromBounds({-19.21f,-1.26f,-11.83f},{18.00f,14.29f,11.05f},3.f);
                meta<<std::setprecision(9)<<"{\"frame\":"<<frameIndex_<<",\"budget\":"<<budget<<",\"updates\":"<<probePlan.indices.size()
                    <<",\"probes\":"<<ages.size()<<",\"depth_floats\":"<<depthHistory.values.size()<<",\"source_frame\":"<<depthHistory.sourceFrame
                    <<",\"completed\":"<<(depthHistory.completed?"true":"false")<<",\"camera\":["<<cameraPos_.x<<","<<cameraPos_.y<<","<<cameraPos_.z
                    <<"],\"origin\":["<<grid.origin.x<<","<<grid.origin.y<<","<<grid.origin.z<<"],\"capped_age\":"<<(std::getenv("DDGI_AGE_CAP")?"true":"false")<<"}\n";
                raw.close();meta.close();CaptureRequire(raw.good()&&meta.good(),"probe schedule witness failed");
            }
        }
        probeUpdatesTotal_+=probePlan.indices.size();
        if(std::getenv("SPONZA_PROBE_SCHEDULE_AUDIT")){
            auto hash=[](const auto& values){u64 h=14695981039346656037ull;for(auto v:values){u32 bits;if constexpr(std::is_same_v<decltype(v),float>)bits=std::bit_cast<u32>(v);else bits=u32(v);for(u32 k=0;k<4;++k){h^=(bits>>(8*k))&255u;h*=1099511628211ull;}}return h;};
            PRIMAL_LOG_STREAM(Info,"DawnProbeSchedule") << "frame=" << frameIndex_ << " budget=" << budget << " updates=" << probePlan.indices.size()
                << " cumulative=" << probeUpdatesTotal_ << " source=" << depthHistory.sourceFrame << " completed=" << depthHistory.completed
                << " list_hash=" << hash(probePlan.indices) << " score_hash=" << hash(probePlan.scores) << " age_hash=" << hash(probeSchedule_.LastUpdates());
        }
    }
    auto cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmd == INVALID_COMMAND_BUFFER) return;

    auto* dawnCmd = device_->GetCommandBuffer(cmd);
    if (!dawnCmd) return;
    CaptureRequire(dawnCmd->Begin(), "command begin failed");
    const char* geometryCapturePrefix=std::getenv("SPONZA_GEOMETRY_CAPTURE_PREFIX");
    const bool geometryCapture=measuredGeometryEnabled_ && geometryCapturePrefix &&
        (frameIndex_<3 || frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350));
    const bool surfacePreviouslyCovered=surfaceCacheEnabled_&&surfaceCache_.RecordedAllCards();
    if(surfaceCacheEnabled_)CaptureRequire(surfaceCache_.Record(*dawnCmd),"surface cache encoding failed");
    const char* surfaceCapturePrefix=std::getenv("SPONZA_SURFACE_CAPTURE_PREFIX");
    const bool surfaceCaptureSelected=frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350);
    const char* surfaceCycle=std::getenv("SPONZA_SURFACE_CAPTURE_CYCLE");
    const bool surfaceCaptureFirst=surfaceCycle&&surfaceCycle[0]=='1'&&!surfacePreviouslyCovered&&surfaceCache_.RecordedAllCards();
    const bool surfaceCapture=surfaceCacheEnabled_&&surfaceCapturePrefix&&(surfaceCaptureSelected||surfaceCaptureFirst);
    const std::string surfaceCapturePath=surfaceCapture?std::string(surfaceCapturePrefix)+(surfaceCaptureSelected?"":"-first"):std::string{};
    if(surfaceCapture)CaptureRequire(surfaceCache_.Capture(*dawnCmd),"surface cache readback requires all cards");
    if(measuredGeometryEnabled_){
        CaptureRequire(probeUpdateEnabled_?measuredGeometry_.RecordSelected(*dawnCmd,frameIndex_,probePlan.indices):measuredGeometry_.Record(*dawnCmd,frameIndex_),"measured geometry encoding failed");
        if(probeUpdateEnabled_)CaptureRequire(probeSchedule_.Commit(frameIndex_),"probe schedule commit failed");
        if(geometryCapture)CaptureRequire(measuredGeometry_.Capture(*dawnCmd,frameIndex_),"geometry readback encoding failed");
    }

    primal::tests::dawn_sponza::ShadowParameters shadowParams{};
    shadowParams.inverseViewProjection = rhimath::Inverse(viewProj);
    // Match the frozen pipeline normalization before the shadow module fit.
    const auto shadowLightDirection = rhimath::Normalize(rhimath::v3{0.5f,-0.7f,0.3f});
    shadowParams.lightDirection = {shadowLightDirection.x,shadowLightDirection.y,shadowLightDirection.z,0};
    shadowParams.width=width_; shadowParams.height=height_; shadowParams.depthTexelSize={1.f/width_,1.f/height_};
    shadowParams.sunTanHalfAngle=std::tan(0.5357f*0.5f*rhimath::constants::DEG_TO_RAD);
    shadowParams.quality=std::getenv("PRIMAL_SHADOW_WHITE")?255u:2u;
    primal::tests::dawn_sponza::FitShadowCascades(shadowParams,shadowBounds_,2048);
    if(frameIndex_==0){
        PRIMAL_LOG_STREAM(Info,"DawnSponza") << "Shadow fit: depth_span=" << shadowParams.cascade[0].y
            << " texel_world=" << shadowParams.cascade[0].x << " sun_tan_half=" << shadowParams.sunTanHalfAngle << std::endl;
        if(std::getenv("PRIMAL_DUMP_MATRIX"))for(u32 cascade=0;cascade<4;++cascade){
            std::ostringstream line;line<<"[DawnShadowMatrix] cascade="<<cascade<<std::setprecision(9);
            for(int col=0;col<4;++col)for(int row=0;row<4;++row)line<<" "<<primal::math::elem(shadowParams.lightVP[cascade],col,row);
            PRIMAL_LOG_STREAM(Info,"DawnSponza")<<line.str()<<std::endl;
        }
    }
    CaptureRequire(shadows_.RecordMaps(*dawnCmd,shadowParams,frameIndex_,[&](DawnCommandBuffer& c){
        for(const auto& mesh:sceneMeshes_)if(mesh.mesh)mesh.mesh->Draw(&c,1,0,0);
    }),"shadow raster encoding failed");
    const char* lightingCapturePrefix=std::getenv("SPONZA_SURFACE_LIGHTING_CAPTURE_PREFIX");
    const bool lightingCapture=surfaceLightingEnabled_ && lightingCapturePrefix && frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350);
    const char* tileCapturePrefix=std::getenv("SPONZA_TILE_CAPTURE_PREFIX");
    const bool tileCapture=tileRadiosityEnabled_ && tileCapturePrefix && frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350);
    if(tileRadiosityEnabled_)CaptureRequire(tileRadiosity_.Prepare(*dawnCmd,frameIndex_),"tile preparation failed");
    if(tileCapture)CaptureRequire(tileRadiosity_.CaptureLightInput(*dawnCmd,frameIndex_),"tile lighting input capture failed");
    if(surfaceLightingEnabled_){
        CaptureRequire(surfaceLighting_.Record(*dawnCmd,frameIndex_,shadowParams.lightVP,tileRadiosityEnabled_?tileRadiosity_.LightInput(frameIndex_):handles::INVALID_RESOURCE,tileRadiosityEnabled_&&tileRadiosity_.Active(frameIndex_)),"surface lighting encoding failed");
        if(lightingCapture)CaptureRequire(surfaceLighting_.Capture(*dawnCmd,frameIndex_),"surface lighting readback encoding failed");
    }
    if(tileRadiosityEnabled_){
        CaptureRequire(tileRadiosity_.Record(*dawnCmd,frameIndex_,surfaceLighting_.FeedbackRadiance(frameIndex_)),"tile radiosity encoding failed");
        if(tileCapture)CaptureRequire(tileRadiosity_.Capture(*dawnCmd,frameIndex_),"tile radiosity capture failed");
    }
    const char* radianceCapturePrefix=std::getenv("SPONZA_MEASURED_RADIANCE_CAPTURE_PREFIX");
    const bool radianceCapture=measuredRadianceEnabled_ && radianceCapturePrefix && (frameIndex_<3 || frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350));
    if(measuredRadianceEnabled_){
        CaptureRequire(measuredRadiance_.Record(*dawnCmd,frameIndex_,measuredGeometry_.GetRecordedOutput(frameIndex_),surfaceLighting_.Radiance(frameIndex_)),"measured radiance encoding failed");
        if(radianceCapture)CaptureRequire(measuredRadiance_.Capture(*dawnCmd,frameIndex_),"measured radiance readback encoding failed");
    }
    const char* probeCapturePrefix=std::getenv("SPONZA_PROBE_UPDATE_CAPTURE_PREFIX");
    const bool probeCapture=probeUpdateEnabled_ && probeCapturePrefix && (frameIndex_<5 || frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350));
    if(probeUpdateEnabled_){
        CaptureRequire(probeUpdate_.Record(*dawnCmd,frameIndex_,measuredGeometry_.GetRecordedOutput(frameIndex_),measuredRadiance_.GetRecordedOutput(frameIndex_),probeCapture),"probe update encoding failed");
        CaptureRequire(probeDepthHistory_.Record(*dawnCmd,frameIndex_,probeUpdate_.GetRecordedOutput(frameIndex_)),"probe depth copy encoding failed");
    }
    auto drawScene = [&] {
        DescriptorSetHandle globalSets[]{globalSets_[frameIdx]};
        dawnCmd->BindDescriptorSets(PipelineBindPoint::Graphics,pipelineLayout_,0,1,globalSets,0,nullptr);
        for(u32 i=0;i<sceneMeshes_.size();++i){
            if(!sceneMeshes_[i].mesh)continue;
            const auto model=rhimath::MatrixIdentity();
            dawnCmd->PushConstants(pipelineLayout_,ShaderStage::Vertex,0,sizeof(model),&model);
            DescriptorSetHandle material[]{meshMaterials_[i].materialSet};
            dawnCmd->BindDescriptorSets(PipelineBindPoint::Graphics,pipelineLayout_,1,1,material,0,nullptr);
            sceneMeshes_[i].mesh->Draw(dawnCmd,1,0,0);
        }
    };
    // WebGPU validates every bound resource in a render-pass scope, even
    // bindings unused by the active shader entry. Keep GBuffer attachments
    // out of the prepass bind group; bind their real views only for consumers.
    auto bindDeferredMaterials=[&](bool consume){
        if(!screenProbePlacementEnabled_)return;
        ResourceHandle textures[]{consume?ormTexture_:directTexture_,consume?albedoTexture_:directTexture_};
        DescriptorImageInfo images[2]{};WriteDescriptorSet writes[2]{};
        for(u32 i=0;i<2;++i){images[i]={handles::INVALID_SAMPLER,textures[i],ResourceState::ShaderResource};writes[i].dstSet=globalSets_[frameIdx];writes[i].dstBinding=12+i;writes[i].descriptorCount=1;writes[i].descriptorType=DescriptorType::SampledImage;writes[i].imageInfo=&images[i];}
        device_->UpdateDescriptorSets(2,writes);
    };
    bindDeferredMaterials(false);
    RenderPassDesc prepass{};prepass.colorAttachments.resize(1);
    prepass.colorAttachments[0].texture=normalTexture_;prepass.colorAttachments[0].format=DataFormat::BGRA8_UNorm;
    prepass.colorAttachments[0].loadOp=LoadAction::Clear;prepass.colorAttachments[0].storeOp=StoreAction::Store;
    prepass.colorAttachments[0].clearValue.color={.5f,.5f,.5f,1};
    if(screenGatherEnabled_){auto orm=prepass.colorAttachments[0];orm.texture=ormTexture_;orm.clearValue.color={1,.8f,0,1};prepass.colorAttachments.push_back(orm);}
    if(screenProbePlacementEnabled_){auto base=prepass.colorAttachments[0];base.texture=albedoTexture_;base.format=DataFormat::RGBA8_UNorm;base.clearValue.color={0,0,0,0};prepass.colorAttachments.push_back(base);}
    prepass.depthAttachment.texture=depthTexture_;prepass.depthAttachment.format=DataFormat::D32_Float;
    prepass.depthAttachment.loadOp=LoadAction::Clear;prepass.depthAttachment.storeOp=StoreAction::Store;prepass.depthAttachment.clearValue.depth=1;
    prepass.viewport={{0,0},{float(width_),float(height_)},0,1};prepass.scissor={{0,0},{width_,height_}};
    dawnCmd->BeginRenderPass(prepass);dawnCmd->BindGraphicsPipeline(normalPipeline_);drawScene();dawnCmd->EndRenderPass();
    CaptureRequire(shadows_.RecordFilter(*dawnCmd,depthTexture_,normalTexture_,shadowParams,frameIndex_),"shadow filter encoding failed");
    // The frozen SPGI color input is CURRENT deferred lighting, before GI.
    // Current pre-GI direct color feeds every frame of the completed SH producer chain.
    const char* deferredPrefix=std::getenv("SPONZA_DEFERRED_CAPTURE_PREFIX");
    const bool deferredCapture=screenProbePlacementEnabled_&&deferredPrefix&&(frameIndex_==0||frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350));
    if(screenProbePlacementEnabled_){
        primal::tests::dawn_sponza::DeferredLighting::Params p{};p.camera[0]=cameraPos_.x;p.camera[1]=cameraPos_.y;p.camera[2]=cameraPos_.z;
        p.lightDirection[0]=.5f;p.lightDirection[1]=-.7f;p.lightDirection[2]=.3f;p.lightDirection[3]=3;
        p.lightColor[0]=p.lightColor[1]=p.lightColor[2]=1;
        CaptureRequire(deferredLighting_.Record(*dawnCmd,frameIndex_,p,raster),"deferred direct encoding failed");
        if(deferredCapture)CaptureRequire(deferredLighting_.Capture(*dawnCmd),"deferred direct capture failed");
    }
    const char* placementPrefix=std::getenv("SPONZA_SCREEN_PROBE_CAPTURE_PREFIX");
    const bool placementCapture=screenProbePlacementEnabled_&&placementPrefix&&(frameIndex_==0||frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350));
    const char* averagePrefix=std::getenv("SPONZA_AVERAGE_CAPTURE_PREFIX");
    const bool averageCapture=placementCapture||(screenProbePlacementEnabled_&&averagePrefix&&frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350));
    const bool historyCapture=screenProbePlacementEnabled_&&placementPrefix&&(placementCapture||frameIndex_<4||frameIndex_+2==u32(captureFrame?std::atoi(captureFrame):350));
    if(screenProbePlacementEnabled_){
        primal::tests::dawn_sponza::ScreenProbePlacement::Params p{};p.viewProjection=viewProj;p.inverseVP=rhimath::Inverse(viewProj);
        p.camera[0]=cameraPos_.x;p.camera[1]=cameraPos_.y;p.camera[2]=cameraPos_.z;p.camera[3]=float(frameIndex_);
        p.trace[0]=50;p.trace[1]=2;p.cache[0]=30;p.cache[1]=1;p.cache[2]=.35f;p.hint[2]=1;
        const auto& sdf=probeRelocation_.Parameters();p.sdfVoxels[0][0]=std::max({sdf.extent[0],sdf.extent[1],sdf.extent[2]})/float(sdf.counts[0]);
        // Frozen host overwrites placement jitter amplitude with zero-hit flags.
        // The selected baseline has zero-hit geometry off, hence this is zero.
        p.surfaceCache[3]=0;
        p.surfaceCache[0]=2048;p.surfaceCache[1]=float(surfaceCache_.CardData().size());p.surfaceCache[2]=1;
        const auto fallbackGrid=graphics::lumen::DDGIFitGridFromBounds({-19.21f,-1.26f,-11.83f},{18.00f,14.29f,11.05f},3.f);
        p.ddgi[0][0]=fallbackGrid.origin.x;p.ddgi[0][1]=fallbackGrid.origin.y;p.ddgi[0][2]=fallbackGrid.origin.z;p.ddgi[0][3]=3;
        p.ddgi[1][0]=float(fallbackGrid.count_x);p.ddgi[1][1]=float(fallbackGrid.count_y);p.ddgi[1][2]=float(fallbackGrid.count_z);p.ddgi[1][3]=1;
        p.diagnostics[1]=.25f;p.tierCounts[1]=8;p.tierCounts[3]=1;p.hint[3]=2;
        CaptureRequire(screenProbes_.RecordPlace(*dawnCmd,frameIndex_,p),"SPGI placement failed");
        // Only reset uses zero SH; subsequent proposals are the actual final
        // Temporal -> measured Spatial result from the preceding frame.
        CaptureRequire(screenProbeHZB_.Build(*dawnCmd,frameIndex_),"SPGI HZB build failed");
        {
            CaptureRequire(screenProbes_.RecordRays(*dawnCmd,frameIndex_,screenProbeHistory_.ProposalForFrame(frameIndex_)),"SPGI history-qualified ray generation failed");
            CaptureRequire(screenProbeHZB_.Trace(*dawnCmd,frameIndex_,screenProbes_.GetRays(frameIndex_)),"SPGI HZB trace failed");
            if(placementCapture)CaptureRequire(screenProbeHZB_.Capture(*dawnCmd,frameIndex_),"SPGI HZB capture failed");
            CaptureRequire(screenProbeSDF_.Record(*dawnCmd,frameIndex_,screenProbes_.GetRays(frameIndex_),screenProbeHZB_.GetOutput(frameIndex_)),"SPGI SDF trace failed");
            if(placementCapture)CaptureRequire(screenProbeSDF_.Capture(*dawnCmd,frameIndex_),"SPGI SDF capture failed");
            primal::tests::dawn_sponza::ScreenProbeFinalize::Frame sources{};
            sources.color=directTexture_;sources.colorFrame=frameIndex_;sources.lighting=surfaceLighting_.Radiance(frameIndex_);sources.lightingFrame=frameIndex_;
            sources.origins=probeRelocation_.Origins();sources.originsGeneration=probeRelocation_.Generation();sources.sceneDigest=measuredGeometry_.SceneDigest();sources.history=probeUpdate_.GetHistoryForFrame(frameIndex_);
            CaptureRequire(screenProbeFinalize_.Record(*dawnCmd,frameIndex_,screenProbes_.GetRays(frameIndex_),screenProbeSDF_.GetOutput(frameIndex_),sources),"SPGI Finalize failed");
            if(placementCapture)CaptureRequire(screenProbeFinalize_.Capture(*dawnCmd,frameIndex_),"SPGI Finalize capture failed");
            CaptureRequire(screenProbeAverage_.Record(*dawnCmd,frameIndex_,screenProbeFinalize_.GetOutput(frameIndex_)),"SPGI Average failed");
            if(averageCapture)CaptureRequire(screenProbeAverage_.Capture(*dawnCmd,frameIndex_),"SPGI Average capture failed");
            CaptureRequire(screenProbeHistory_.Record(*dawnCmd,frameIndex_,screenProbeAverage_.GetOutput(frameIndex_),measuredGeometry_.GetSceneView()),"SPGI Temporal/Spatial history failed");
            if(historyCapture)CaptureRequire(screenProbeHistory_.Capture(*dawnCmd,frameIndex_),"SPGI history capture failed");
            CaptureRequire(screenProbeGather_.Record(*dawnCmd,frameIndex_,screenProbes_.GetRays(frameIndex_).params,screenProbeHistory_.GetOutput(frameIndex_)),"SPGI Gather/Denoise failed");
            if(placementCapture)CaptureRequire(screenProbeGather_.Capture(*dawnCmd),"SPGI Gather/Denoise capture failed");
            if(placementCapture&&std::getenv("SPONZA_SPGI_FINALIZE_INPUTS"))CaptureRequire(screenProbeFinalize_.CaptureInputs(*dawnCmd,frameIndex_),"SPGI Finalize input witness failed");
        }
        if(placementCapture)CaptureRequire(screenProbes_.Capture(*dawnCmd,frameIndex_),"SPGI placement capture failed");
    }

    const char* gatherCapturePrefix=std::getenv("SPONZA_GATHER_CAPTURE_PREFIX");
    const bool gatherCapture=screenGatherEnabled_&&gatherCapturePrefix&&frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350);
    if(screenGatherEnabled_){
        primal::tests::dawn_sponza::ScreenGather::Params p{};p.inverseVP=rhimath::Inverse(viewProj);p.camera[0]=cameraPos_.x;p.camera[1]=cameraPos_.y;p.camera[2]=cameraPos_.z;
        const auto grid=graphics::lumen::DDGIFitGridFromBounds({-19.21f,-1.26f,-11.83f},{18.00f,14.29f,11.05f},3.f);
        p.originSpacing[0]=grid.origin.x;p.originSpacing[1]=grid.origin.y;p.originSpacing[2]=grid.origin.z;p.originSpacing[3]=3;
        p.counts[0]=grid.count_x;p.counts[1]=grid.count_y;p.counts[2]=grid.count_z;
        p.jitterPolicy[0]=rasterJitter.x;p.jitterPolicy[1]=rasterJitter.y;
        if(const char* e=std::getenv("PRIMAL_GI_OPENSKY"))p.jitterPolicy[2]=e[0]!='0';
        if(const char* e=std::getenv("PRIMAL_GI_OPENSKY_V2"))p.jitterPolicy[3]=e[0]!='0';
        CaptureRequire(screenGather_.Record(*dawnCmd,frameIndex_,p,probeUpdate_.GetRecordedOutput(frameIndex_),probeRelocation_.Origins(),probeRelocation_.Generation()),"screen gather encoding failed");
        if(gatherCapture)CaptureRequire(screenGather_.Capture(*dawnCmd),"screen gather capture failed");
    }



    const char* environmentPrefix=std::getenv("SPONZA_ENVIRONMENT_CAPTURE_PREFIX");
    const bool environmentCapture=screenProbePlacementEnabled_&&environmentPrefix&&(frameIndex_==0||frameIndex_==4||frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350));
    if(screenProbePlacementEnabled_){
        primal::tests::dawn_sponza::EnvironmentVisibility::Params p{};p.camera[0]=cameraPos_.x;p.camera[1]=cameraPos_.y;p.camera[2]=cameraPos_.z;p.policy[0]=1;
        if(const char* e=std::getenv("PRIMAL_ENV_VIS_TEMPORAL"))p.target[2]=std::strcmp(e,"0")!=0;
        if(const char* e=std::getenv("PRIMAL_ENV_VIS_BLEND"))p.target[3]=std::strcmp(e,"1")==0;
        CaptureRequire(environmentVisibility_.Record(*dawnCmd,frameIndex_,p,raster,measuredGeometry_.GetSceneView()),"environment visibility encoding failed");
        if(environmentCapture)CaptureRequire(environmentVisibility_.Capture(*dawnCmd,frameIndex_),"environment visibility capture failed");
        primal::tests::dawn_sponza::EnvironmentColor::Params colorParams{};colorParams.inverseVP=rhimath::Inverse(viewProj);colorParams.camera[0]=cameraPos_.x;colorParams.camera[1]=cameraPos_.y;colorParams.camera[2]=cameraPos_.z;
        CaptureRequire(environmentColor_.Record(*dawnCmd,frameIndex_,colorParams,raster),"environment color encoding failed");
        DescriptorImageInfo image{handles::INVALID_SAMPLER,environmentVisibility_.Output(frameIndex_),ResourceState::ShaderResource};WriteDescriptorSet write{};write.dstSet=globalSets_[frameIdx];write.dstBinding=8;write.descriptorCount=1;write.descriptorType=DescriptorType::SampledImage;write.imageInfo=&image;device_->UpdateDescriptorSets(1,&write);
        if(environmentCapture)CaptureRequire(environmentColor_.Capture(*dawnCmd),"environment color capture failed");
    }

    const char* fusionPrefix=std::getenv("SPONZA_FUSION_CAPTURE_PREFIX");
    const bool fusionCapture=screenProbePlacementEnabled_ && fusionPrefix && frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350);
    if (screenProbePlacementEnabled_) {
        using Fusion = primal::tests::dawn_sponza::Fusion;
        auto inputs=Fusion::EmptyInputs();
        inputs[Fusion::DDGI]=screenGather_.Diffuse();
        inputs[Fusion::SPGI]=screenProbeGather_.Filtered();
        inputs[Fusion::Albedo]=albedoTexture_;
        inputs[Fusion::Direct]=directTexture_;
        inputs[Fusion::ORM]=ormTexture_;
        inputs[Fusion::Specular]=screenGather_.Specular();
        inputs[Fusion::Environment]=environmentColor_.Output();
        inputs[Fusion::Visibility]=environmentVisibility_.Output(frameIndex_);
        CaptureRequire(fusion_.Record(*dawnCmd,frameIndex_,inputs,fusionParameters_),"two-pass fullscreen fusion failed");
        if (fusionCapture) CaptureRequire(fusion_.Capture(*dawnCmd),"fusion input/output capture failed");
    } else {
        RenderPassDesc rpDesc{};
        rpDesc.colorAttachments.resize(1);
        rpDesc.colorAttachments[0].texture = hdrTexture_;
        rpDesc.colorAttachments[0].format = DataFormat::RGBA16_Float;
        rpDesc.colorAttachments[0].loadOp = LoadAction::Clear;
        rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
        rpDesc.colorAttachments[0].clearValue.color = {0.1f, 0.1f, 0.15f, 1.0f};  // Dark background

        rpDesc.depthAttachment.texture = depthTexture_;
        rpDesc.depthAttachment.format = DataFormat::D32_Float;
        rpDesc.depthAttachment.loadOp = LoadAction::Load;
        rpDesc.depthAttachment.storeOp = StoreAction::Store;
        rpDesc.depthAttachment.clearValue.depth = 1.0f;

        rpDesc.viewport = ViewportDesc{{0.0f, 0.0f}, {(float)width_, (float)height_}, 0.0f, 1.0f};
        rpDesc.scissor = primal::graphics::rhi::Rect{{0, 0}, {width_, height_}};

        bindDeferredMaterials(true);
        dawnCmd->BeginRenderPass(rpDesc);
        dawnCmd->BindGraphicsPipeline(pipeline_);

        drawScene();

        dawnCmd->EndRenderPass();
    }
    const char* taaPrefix=std::getenv("SPONZA_TAA_CAPTURE_PREFIX");
    const bool taaCapture=taaEnabled_&&taaPrefix&&(frameIndex_==0||frameIndex_+1==u32(captureFrame?std::atoi(captureFrame):350));
    ResourceHandle resolvedHDR=hdrTexture_;
    if(taaEnabled_){CaptureRequire(temporalAA_.Record(*dawnCmd,frameIndex_,hdrTexture_,raster),"TAA paired history resolve failed");resolvedHDR=temporalAA_.Output(frameIndex_);if(taaCapture)CaptureRequire(temporalAA_.Capture(*dawnCmd,frameIndex_),"TAA capture failed");}
    static auto previousToneTime = std::chrono::steady_clock::now();
    const auto toneTime = std::chrono::steady_clock::now();
    const float toneDt = frameIndex_ == 0 ? 0.016f : std::clamp(
        std::chrono::duration<float>(toneTime - previousToneTime).count(), 0.001f, 0.25f);
    previousToneTime = toneTime;
    const primal::tests::dawn_sponza::AdaptParameters adapt{
        1.f - std::exp(-toneDt / 0.30f), 1.f - std::exp(-toneDt / 1.00f)};
    CaptureRequire(postProcess_.Record(*dawnCmd, resolvedHDR, ldrTexture_==INVALID_RESOURCE?backBuffer:ldrTexture_, frameIndex_, toneParameters_, adapt), "HDR postprocess encoding failed");
    if(ldrTexture_!=INVALID_RESOURCE)CaptureRequire(present_.Record(*dawnCmd,backBuffer),"LDR FinalBlit failed");
    if (capture) {
        BufferTextureCopyRegion region{};
        region.bufferRowLength = rowBytes / 4; // RHI texels, not WebGPU bytes.
        region.bufferImageHeight = outputHeight_;
        region.imageExtent = {outputWidth_, outputHeight_, 1};
        dawnCmd->CopyTextureToBuffer(captureTexture, readback, &region, 1);
        BufferTextureCopyRegion stateRegion{}; stateRegion.imageExtent = {1,1,1};
        dawnCmd->CopyTextureToBuffer(postProcess_.CurrentState(), adaptReadback, &stateRegion, 1);
        if(ldrReadback!=INVALID_RESOURCE){BufferTextureCopyRegion r{};r.bufferRowLength=ldrPitch/4;r.bufferImageHeight=height_;r.imageExtent={width_,height_,1};dawnCmd->CopyTextureToBuffer(ldrTexture_,ldrReadback,&r,1);}
    }
    CaptureRequire(dawnCmd->End(), "command end failed");

    QueueSubmitInfo submit{};
    submit.cmdBuffer = cmd;
    CaptureRequire(device_->Submit(submit), "render submission failed");
    if(probeUpdateEnabled_)CaptureRequire(probeDepthHistory_.Submitted(frameIndex_),"probe depth mapping request failed");
    if(fusionCapture || deferredCapture || environmentCapture || taaCapture || historyCapture || averageCapture || placementCapture || gatherCapture || tileCapture || geometryCapture || surfaceCapture || lightingCapture || radianceCapture || probeCapture || capture)CaptureRequire(dawnCmd->WaitForCompletion(),"capture GPU completion failed");
    if(fusionCapture)CaptureRequire(fusion_.Save(std::string(fusionPrefix)+"-"+std::to_string(frameIndex_)),"fusion capture validation failed");
    if(deferredCapture)CaptureRequire(deferredLighting_.Save(std::string(deferredPrefix)+"-"+std::to_string(frameIndex_)),"deferred capture validation failed");
    if(environmentCapture)CaptureRequire(environmentColor_.Save(std::string(environmentPrefix)+"-color-"+std::to_string(frameIndex_)),"environment color capture validation failed");
    if(environmentCapture)CaptureRequire(environmentVisibility_.Save(std::string(environmentPrefix)+"-"+std::to_string(frameIndex_)),"environment visibility finite validation failed");
    if(taaCapture)CaptureRequire(temporalAA_.Save(std::string(taaPrefix)+"-"+std::to_string(frameIndex_)),"TAA finite verification failed");
    if(placementCapture)CaptureRequire(screenProbeSDF_.Save(std::string(placementPrefix)+"-sdf-"+std::to_string(frameIndex_)),"SPGI SDF validity check failed");
    if(placementCapture)CaptureRequire(screenProbeFinalize_.Save(std::string(placementPrefix)+"-finalize-"+std::to_string(frameIndex_)),"SPGI Finalize validity check failed");
    if(averageCapture)CaptureRequire(screenProbeAverage_.Save(std::string(placementCapture?placementPrefix:averagePrefix)+"-average-"+std::to_string(frameIndex_)),"SPGI Average validity check failed");
    if(placementCapture)CaptureRequire(screenProbeHZB_.Save(std::string(placementPrefix)+"-hzb-"+std::to_string(frameIndex_)),"SPGI HZB finite check failed");
    if(historyCapture)CaptureRequire(screenProbeHistory_.Save(std::string(placementPrefix)+"-history-"+std::to_string(frameIndex_)),"SPGI final history validity failed");
    if(placementCapture)CaptureRequire(screenProbeGather_.Save(std::string(placementPrefix)+"-gather-"+std::to_string(frameIndex_)),"SPGI Gather/Denoise validity failed");
    if(placementCapture)CaptureRequire(screenProbes_.Save(std::string(placementPrefix)+"-"+std::to_string(frameIndex_)),"SPGI placement finite check failed");
    if(gatherCapture)CaptureRequire(screenGather_.Save(gatherCapturePrefix),"screen gather finite validation failed");
    if(tileCapture)CaptureRequire(tileRadiosity_.Save(tileCapturePrefix),"tile radiosity validation failed");
    if(probeCapture)CaptureRequire(probeUpdate_.VerifyAndSave(std::string(probeCapturePrefix)+"-"+std::to_string(frameIndex_)),"probe update validation failed");
    if(radianceCapture)CaptureRequire(measuredRadiance_.VerifyAndSave(std::string(radianceCapturePrefix)+"-"+std::to_string(frameIndex_)),"measured radiance validation failed");
    if(lightingCapture)CaptureRequire(surfaceLighting_.VerifyAndSave(lightingCapturePrefix),"surface lighting validation failed");
    if(surfaceCapture)CaptureRequire(surfaceCache_.VerifyAndSave(surfaceCapturePath.c_str()),"surface cache material validation failed");
    if(geometryCapture){
        CaptureRequire(measuredGeometry_.VerifyAndSave(std::string(geometryCapturePrefix)+"-"+std::to_string(frameIndex_),frameIndex_),"geometry CPU oracle comparison failed");
    }
    if (capture) {
        const auto* state = static_cast<const float*>(device_->MapBuffer(adaptReadback, 0, 4));
        CaptureRequire(state && std::isfinite(*state), "adaptation witness invalid");
        PRIMAL_LOG_STREAM(Info, "DawnSponza") << "[DawnToneWitness] frame=" << frameIndex_ + 1
            << " log2Lum=" << *state << " avgLum=" << std::exp2(*state)
            << " effectiveExposure=" << toneParameters_.exposure * (toneParameters_.autoAdapt > 0.5f ? toneParameters_.key / std::exp2(*state) : 1.f)
            << std::endl;
        device_->UnmapBuffer(adaptReadback); device_->DestroyBuffer(adaptReadback);
        if(ldrReadback!=INVALID_RESOURCE){const auto* raw=static_cast<const char*>(device_->MapBuffer(ldrReadback,0,u64(ldrPitch)*height_));CaptureRequire(raw,"LDR capture map failed");std::ofstream f(ldrCapturePath,std::ios::binary);for(u32 y=0;y<height_;++y)f.write(raw+u64(y)*ldrPitch,width_*4);f.close();CaptureRequire(f.good(),"LDR capture save failed");device_->UnmapBuffer(ldrReadback);device_->DestroyBuffer(ldrReadback);}
        const auto* pixels = static_cast<const unsigned char*>(device_->MapBuffer(readback, 0, u64(rowBytes) * outputHeight_));
        CaptureRequire(pixels, "capture mapping failed");
        std::ofstream file(capturePath, std::ios::binary);
        file << "P6\n" << outputWidth_ << " " << outputHeight_ << "\n255\n";
        for (u32 y = 0; y < outputHeight_; ++y) {
            for (u32 x = 0; x < outputWidth_; ++x) {
                const auto* pixel = pixels + u64(y) * rowBytes + x * 4;
                const char rgb[3] = {char(pixel[2]), char(pixel[1]), char(pixel[0])};
                file.write(rgb, 3);
            }
        }
        file.close();
        const bool saved = bool(file);
        device_->UnmapBuffer(readback);
        device_->DestroyBuffer(readback); device_->DestroyTexture(captureTexture);
        CaptureRequire(saved, "capture file write failed");
        PRIMAL_LOG_STREAM(Info, "DawnSponza") << "saved " << capturePath << " frame=" << frameIndex_ + 1 << std::endl;
    }

    swapchain_->Present(INVALID_SYNC);

    device_->DestroyCommandBuffer(cmd);
    device_->EndFrame();

    timer_.end();

    // Log FPS every 60 frames
    static u32 fpsCounter = 0;
    static auto fpsStartTime = std::chrono::steady_clock::now();
    fpsCounter++;
    if (fpsCounter >= 60) {
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - fpsStartTime).count();
        float fps = fpsCounter / elapsed;
        std::cerr << "[FPS] " << fps << std::endl;
        fpsCounter = 0;
        fpsStartTime = now;
    }

    frameIndex_++;
    if (const char* maximum = std::getenv("SPONZA_MAX_FRAMES")) {
        if (std::atoi(maximum) > 0 && frameIndex_ >= u32(std::atoi(maximum))) {
            PRIMAL_LOG_STREAM(Info, "DawnSponza") << "clean bounded exit at frame " << frameIndex_ << std::endl;
            std::_Exit(0);
        }
    }
}

// ============================================================
// Shutdown
// ============================================================

void Engine_Test::shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    std::cout << "[DawnSponza] Shutting down..." << std::endl;

    if (displayLink_) {
        CFRunLoopRemoveTimer(runLoop_, displayLink_, kCFRunLoopCommonModes);
        CFRelease(displayLink_);
        displayLink_ = nullptr;
    }

    for (auto& mat : meshMaterials_) {
        if (mat.materialSet != INVALID_DESCRIPTOR_SET && device_) device_->DestroyDescriptorSet(mat.materialSet);
    }
    meshMaterials_.clear();

    // Destroy cached textures (each unique texture is destroyed once)
    for (auto& [path, handle] : textureCache_) {
        if (handle != INVALID_RESOURCE && device_) device_->DestroyTexture(handle);
    }
    textureCache_.clear();

    for (u32 i = 0; i < kFrameCount; ++i) {
        if (globalSets_[i] != INVALID_DESCRIPTOR_SET && device_) device_->DestroyDescriptorSet(globalSets_[i]);
        if (viewBuffers_[i] != INVALID_RESOURCE && device_) device_->DestroyBuffer(viewBuffers_[i]);
        if (lightBuffers_[i] != INVALID_RESOURCE && device_) device_->DestroyBuffer(lightBuffers_[i]);
    }

    probeDepthHistory_.Shutdown();
    probeSchedule_.Reset();
    probeUpdate_.Shutdown();
    measuredRadiance_.Shutdown();
    fusion_.Shutdown();
    deferredLighting_.Shutdown();
    environmentColor_.Shutdown();
    environmentIBL_.Shutdown();
    if(environmentPolicy_!=INVALID_RESOURCE&&device_)device_->DestroyBuffer(environmentPolicy_);
    if(albedoTexture_!=INVALID_RESOURCE&&device_)device_->DestroyTexture(albedoTexture_);
    environmentVisibility_.Shutdown();
    temporalAA_.Shutdown();
    present_.Shutdown();
    if(ldrTexture_!=INVALID_RESOURCE){device_->DestroyTexture(ldrTexture_);ldrTexture_=INVALID_RESOURCE;}
    screenProbeGather_.Shutdown();
    screenProbeHistory_.Shutdown();
    screenProbeAverage_.Shutdown();
    screenProbeFinalize_.Shutdown();
    screenProbeSDF_.Shutdown();
    screenProbeHZB_.Shutdown();
    screenProbes_.Shutdown();
    screenGather_.Shutdown();
    if(ormTexture_!=INVALID_RESOURCE&&device_)device_->DestroyTexture(ormTexture_);
    tileRadiosity_.Shutdown();
    surfaceLighting_.Shutdown();
    surfaceCache_.Shutdown();
    measuredGeometry_.Shutdown();
    probeRelocation_.Shutdown();
    shadows_.Shutdown();
    if(normalTexture_!=INVALID_RESOURCE && device_)device_->DestroyTexture(normalTexture_);
    if(normalPipeline_!=INVALID_PIPELINE && device_)device_->DestroyPipeline(normalPipeline_);
    if(directTexture_!=INVALID_RESOURCE && device_)device_->DestroyTexture(directTexture_);
    if(normalShader_!=INVALID_SHADER && device_)device_->DestroyShader(normalShader_);
    postProcess_.Shutdown();
    if (hdrTexture_ != INVALID_RESOURCE && device_) device_->DestroyTexture(hdrTexture_);
    if (depthTexture_ != INVALID_RESOURCE && device_) device_->DestroyTexture(depthTexture_);
    if (defaultSampler_ != INVALID_SAMPLER && device_) device_->DestroySampler(defaultSampler_);
    if (pipeline_ != INVALID_PIPELINE && device_) device_->DestroyPipeline(pipeline_);
    if (pipelineLayout_ != INVALID_PIPELINE_LAYOUT && device_) device_->DestroyPipelineLayout(pipelineLayout_);
    if (materialLayout_ != INVALID_DESCRIPTOR_SET_LAYOUT && device_) device_->DestroyDescriptorSetLayout(materialLayout_);
    if (globalLayout_ != INVALID_DESCRIPTOR_SET_LAYOUT && device_) device_->DestroyDescriptorSetLayout(globalLayout_);
    if (vs_ != INVALID_SHADER && device_) device_->DestroyShader(vs_);
    if (fs_ != INVALID_SHADER && device_) device_->DestroyShader(fs_);
    if (swapchain_ && device_) {
        device_->DestroySwapChain(swapchain_);
        swapchain_ = nullptr;
    }
    if (device_) {
        device_->Shutdown();
        delete device_;
        device_ = nullptr;
    }

    content::shutdown();

    std::cerr << "[DawnSponza] window_.is_valid() = " << window_.is_valid()
              << " get_id() = " << (u32)window_.get_id() << std::endl;
    if (window_.is_valid()) {
        platform::remove_window(window_.get_id());
        std::cerr << "[DawnSponza] remove_window called" << std::endl;
    } else {
        std::cerr << "[DawnSponza] window NOT valid, skipping remove_window" << std::endl;
    }

    std::cout << "[DawnSponza] Shutdown complete" << std::endl;
}

// ============================================================
// macOS ApplicationDelegate
// ============================================================

#ifdef __APPLE__

void Engine_Test::applicationDidFinishLaunching(NS::Notification* notification) {
    NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
    pApp->activateIgnoringOtherApps(true);

    if (!initialize()) {
        std::cerr << "[DawnSponza] Initialization failed" << std::endl;
        NS::Application::sharedApplication()->terminate(nullptr);
        return;
    }

    SetupRenderLoop();
}

void Engine_Test::applicationWillFinishLaunching(NS::Notification* notification) {
    NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
    pApp->setActivationPolicy(NS::ActivationPolicy::ActivationPolicyRegular);
}

bool Engine_Test::applicationShouldTerminateAfterLastWindowClosed(
    [[maybe_unused]] NS::Application* pSender) {
    shutdown();
    return true;
}

void Engine_Test::SetupRenderLoop() {
    runLoop_ = CFRunLoopGetMain();
    CFRunLoopTimerContext context = {0, this, nullptr, nullptr, nullptr};
    displayLink_ = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent(),
        1.0 / 60.0,
        0, 0,
        DisplayLinkCallback,
        &context
    );
    CFRunLoopAddTimer(runLoop_, displayLink_, kCFRunLoopCommonModes);
    std::cerr << "[DawnSponza] 60 FPS render loop started" << std::endl;
}

void Engine_Test::DisplayLinkCallback(CFRunLoopTimerRef, void* info) {
    auto* test = static_cast<Engine_Test*>(info);
    test->run();
}

#endif // __APPLE__

#endif // ENABLE_WEBGPU
