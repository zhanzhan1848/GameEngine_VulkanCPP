#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "TestDawnSponza.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Content/ContentToEngine.h"
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#endif

using namespace primal;
using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::handles;

namespace rhimath = primal::graphics::rhi::math;

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
    // Calculate mip level count
    u32 mipLevels = 1;
    { u32 maxDim = std::max((u32)w, (u32)h); while (maxDim > 1) { mipLevels++; maxDim /= 2; } }

    TextureDesc desc{};
    desc.size = {(u32)w, (u32)h, 1};
    desc.format = format;
    desc.type = TextureType::Texture2D;
    desc.mipLevels = mipLevels;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;

    ResourceHandle tex = device_->CreateTexture(desc);
    if (tex == INVALID_RESOURCE) return INVALID_RESOURCE;

    device_->UpdateTextureData(tex, data, 0, 0, 0, (u32)w, (u32)h, 1, 4 * w, 0);

    // Generate and upload mip levels via CPU box filter
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
                    int v00 = srcData[(sy * curW + sx) * 4 + c];
                    int v10 = (sx + 1 < curW) ? srcData[(sy * curW + sx + 1) * 4 + c] : v00;
                    int v01 = (sy + 1 < curH) ? srcData[((sy + 1) * curW + sx) * 4 + c] : v00;
                    int v11 = (sx + 1 < curW && sy + 1 < curH) ? srcData[((sy + 1) * curW + sx + 1) * 4 + c] : v00;
                    mipBuf[(y * nextW + x) * 4 + c] = (unsigned char)((v00 + v10 + v01 + v11 + 2) / 4);
                }
            }
        }

        device_->UpdateTextureData(tex, mipBuf.data(), 0, 0, 0, nextW, nextH, 1, nextW * 4, mip);
        srcData = mipBuf.data();
        curW = nextW;
        curH = nextH;
    }

    return tex;
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

    textureCache_[cacheKey] = tex;
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
    fs_ = device_->CreateShader(shaderSource.data(), shaderSource.size(), ShaderStage::Pixel, "forward_pbr_fs");
    if (vs_ == INVALID_SHADER || fs_ == INVALID_SHADER) {
        std::cerr << "[DawnSponza] Failed to compile shaders" << std::endl;
        return false;
    }

    // Global descriptor set layout: ViewData uniform + LightData uniform
    {
        DescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = DescriptorType::UniformBuffer;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;

        bindings[1].binding = 5;
        bindings[1].descriptorType = DescriptorType::UniformBuffer;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = ShaderStage::Pixel;

        DescriptorSetLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = 2;
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
        pipeDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
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

        pipeline_ = device_->CreateGraphicsPipeline(pipeDesc);
        if (pipeline_ == INVALID_PIPELINE) {
            std::cerr << "[DawnSponza] Failed to create graphics pipeline" << std::endl;
            return false;
        }
    }

    // Default sampler with anisotropic filtering for oblique surfaces
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
        defaultSampler_ = device_->CreateSampler(samplerDesc);
    }

    return true;
}

// ============================================================
// CreateResources
// ============================================================

bool Engine_Test::CreateResources() {
    // Depth texture
    {
        TextureDesc desc{};
        desc.size = {width_, height_, 1};
        desc.format = DataFormat::D32_Float;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
        depthTexture_ = device_->CreateTexture(desc);
        if (depthTexture_ == INVALID_RESOURCE) return false;
    }

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

            WriteDescriptorSet writes[2]{};
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

            device_->UpdateDescriptorSets(2, writes);
        }
    }

    return true;
}

// ============================================================
// LoadScene
// ============================================================

bool Engine_Test::LoadScene() {
    std::string baseDir = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/";

    std::string modelPath = baseDir + "Sponza_process_rebuild.model";
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        modelPath = baseDir + "Sponza.model";
        file.open(modelPath, std::ios::binary | std::ios::ate);
    }
    if (!file.is_open()) {
        std::cerr << "[DawnSponza] Failed to open Sponza model file" << std::endl;
        return false;
    }

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
            mat.diffuseTex = LoadTextureFromFile(diffusePath, DataFormat::RGBA8_UNorm);
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
            mat.diffuseTex = CreateTextureFromData(1, 1, white, DataFormat::RGBA8_UNorm);
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
    std::cout << "[DawnSponza] Initializing..." << std::endl;

    // 1. Create platform window
    platform::window_init_info windowInfo{
        nullptr, nullptr,
        "Dawn WebGPU Sponza",
        100, 100, (s32)width_, (s32)height_
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
    scDesc.width = width_;
    scDesc.height = height_;
    scDesc.format = DataFormat::BGRA8_UNorm;
    scDesc.bufferCount = kFrameCount;

    swapchain_ = device_->CreateSwapChain(scDesc);
    if (!swapchain_) {
        std::cerr << "[DawnSponza] Failed to create swapchain" << std::endl;
        return false;
    }

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
    UpdateCamera(dt);

    // Build view-projection matrix using RHIMath utilities
    float aspectRatio = (float)width_ / (float)height_;

    float cosY = cosf(cameraYaw_), sinY = sinf(cameraYaw_);
    float cosP = cosf(cameraPitch_), sinP = sinf(cameraPitch_);
    rhimath::v3 forward = rhimath::Normalize(rhimath::v3{sinY * cosP, sinP, cosY * cosP});
    rhimath::v3 target = cameraPos_ + forward;

    rhimath::m4x4 viewMatrix = rhimath::CreateLookAtMatrix(cameraPos_, target, rhimath::v3{0.0f, 1.0f, 0.0f});
    rhimath::m4x4 projMatrix = rhimath::MatrixPerspective(60.0f * rhimath::constants::DEG_TO_RAD, aspectRatio, 0.1f, 1000.0f);
    rhimath::m4x4 viewProj = projMatrix * viewMatrix;

    // Update per-frame uniform buffers
    u32 frameIdx = frameIndex_ % kFrameCount;
    {
        struct ViewDataCPU {
            rhimath::m4x4 viewProjection;
            rhimath::m4x4 invViewProjection;
            rhimath::v4 cameraPos;
        };
        ViewDataCPU vd{};
        vd.viewProjection = viewProj;
        vd.invViewProjection = rhimath::Inverse(viewProj);
        vd.cameraPos = rhimath::v4{cameraPos_.x, cameraPos_.y, cameraPos_.z, 0.0f};
        void* mapped = device_->MapBuffer(viewBuffers_[frameIdx], 0, sizeof(ViewDataCPU));
        if (mapped) {
            memcpy(mapped, &vd, sizeof(ViewDataCPU));
            device_->UnmapBuffer(viewBuffers_[frameIdx]);
        } else {
            static bool loggedOnce = false;
            if (!loggedOnce) { std::cerr << "[DawnSponza] MapBuffer FAILED for viewBuffers_[" << frameIdx << "]" << std::endl; loggedOnce = true; }
        }
    }
    {
        struct LightDataCPU {
            rhimath::v4 direction;
            rhimath::v4 color;
        };
        LightDataCPU ld{};
        ld.direction = rhimath::v4{0.5f, -0.7f, 0.3f, 3.0f};
        ld.color = rhimath::v4{1.0f, 0.95f, 0.85f, 0.0f};
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

    auto cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmd == INVALID_COMMAND_BUFFER) return;

    auto* dawnCmd = device_->GetCommandBuffer(cmd);
    if (!dawnCmd) return;
    dawnCmd->Begin();

    RenderPassDesc rpDesc{};
    rpDesc.colorAttachments.resize(1);
    rpDesc.colorAttachments[0].texture = backBuffer;
    rpDesc.colorAttachments[0].format = DataFormat::BGRA8_UNorm;
    rpDesc.colorAttachments[0].loadOp = LoadAction::Clear;
    rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
    rpDesc.colorAttachments[0].clearValue.color = {0.1f, 0.1f, 0.15f, 1.0f};  // Dark background

    rpDesc.depthAttachment.texture = depthTexture_;
    rpDesc.depthAttachment.format = DataFormat::D32_Float;
    rpDesc.depthAttachment.loadOp = LoadAction::Clear;
    rpDesc.depthAttachment.storeOp = StoreAction::Store;
    rpDesc.depthAttachment.clearValue.depthStencil = {1.0f, 0};

    rpDesc.viewport = ViewportDesc{{0.0f, 0.0f}, {(float)width_, (float)height_}, 0.0f, 1.0f};
    rpDesc.scissor = primal::graphics::rhi::Rect{{0, 0}, {width_, height_}};

    dawnCmd->BeginRenderPass(rpDesc);
    dawnCmd->BindGraphicsPipeline(pipeline_);

    // Bind global descriptor set (view + light data)
    DescriptorSetHandle globalSets[] = {globalSets_[frameIdx]};
    dawnCmd->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout_, 0, 1, globalSets, 0, nullptr);

    // Draw each mesh
    for (u32 i = 0; i < sceneMeshes_.size(); ++i) {
        const auto& meshInfo = sceneMeshes_[i];
        if (!meshInfo.mesh) continue;

        rhimath::m4x4 modelMatrix = rhimath::MatrixIdentity();
        dawnCmd->PushConstants(pipelineLayout_, ShaderStage::Vertex, 0, sizeof(rhimath::m4x4), &modelMatrix);

        DescriptorSetHandle matSet[] = {meshMaterials_[i].materialSet};
        dawnCmd->BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayout_, 1, 1, matSet, 0, nullptr);

        meshInfo.mesh->Draw(dawnCmd, 1, 0, 0);
    }

    dawnCmd->EndRenderPass();
    dawnCmd->End();

    QueueSubmitInfo submit{};
    submit.cmdBuffer = cmd;
    device_->Submit(submit);

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
