/**
 * @file ForwardRenderer.cpp
 * @brief Forward Rendering implementation.
 * 
 * Implements a standard forward rendering pipeline with support for:
 * - Depth Pre-pass
 * - Main Render Pass (Opaque)
 * - Transparent Pass
 * - Screen Space Reflection (SSR)
 * - Post-processing (Composite)
 */
#include "ForwardRenderer.h"
#include "Graphics/RenderPipeline/RenderPasses/Debug/GeometryDebugPass.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Components/Pipeline.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIDescriptorSetLayout.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"
#include "Graphics/RHI/Utils/ShadowUtils.h"
#include "Graphics/Scene/SceneExtractionSystem.h"

#include <algorithm>
#include <iostream>
#include <fstream>
#ifdef __EMSCRIPTEN__
#include "Graphics/Dawn/ShaderLoader.h"
#endif

namespace primal::graphics {

constexpr u32 MAX_CSM_CASCADES = 4;
constexpr u32 MAX_SPOT_SHADOWS = 4;
constexpr u32 SHADOW_MAP_ARRAY_SIZE = MAX_CSM_CASCADES + MAX_SPOT_SHADOWS;
constexpr u32 MAX_POINT_SHADOWS = 2;

ForwardRenderer::ForwardRenderer() = default;
ForwardRenderer::~ForwardRenderer() = default;

bool ForwardRenderer::Initialize(rhi::RHIDeviceBase* device) {
    if (!device) return false;
    device_ = device;

    // 1. Create Light Buffers
    rhi::BufferDesc lightBufferDesc{};
    lightBufferDesc.size = sizeof(rhi::ForwardLightBuffer);
    lightBufferDesc.type = rhi::BufferType::Constant;
    lightBufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    lightBufferDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    lightBufferDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ConstantBuffer);

    // 2. Create Frame Buffers (GlobalShaderData)
    rhi::BufferDesc frameBufferDesc{};
    frameBufferDesc.size = sizeof(rhi::GlobalShaderData);
    frameBufferDesc.type = rhi::BufferType::Constant;
    frameBufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    frameBufferDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    frameBufferDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ConstantBuffer);

    // 3. Create Per-Object Buffers
    rhi::BufferDesc perObjectBufferDesc{};
    perObjectBufferDesc.size = MAX_PER_OBJECT_SIZE;
    perObjectBufferDesc.type = rhi::BufferType::Constant; // Using Dynamic Offset
    perObjectBufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    perObjectBufferDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    perObjectBufferDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ConstantBuffer);

    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        // Light Buffer
        lightBuffers_[i] = device->CreateBuffer(lightBufferDesc);
        if (lightBuffers_[i] == rhi::handles::INVALID_RESOURCE) return false;
        lightBuffersMapped_[i] = device->MapBuffer(lightBuffers_[i], 0, sizeof(rhi::ForwardLightBuffer));

        // Frame Buffer
        frameBuffers_[i] = device->CreateBuffer(frameBufferDesc);
        if (frameBuffers_[i] == rhi::handles::INVALID_RESOURCE) return false;
        frameBuffersMapped_[i] = device->MapBuffer(frameBuffers_[i], 0, sizeof(rhi::GlobalShaderData));

        // Per-Object Buffer
        perObjectBuffers_[i] = device->CreateBuffer(perObjectBufferDesc);
        if (perObjectBuffers_[i] == rhi::handles::INVALID_RESOURCE) return false;
        perObjectBuffersMapped_[i] = device->MapBuffer(perObjectBuffers_[i], 0, MAX_PER_OBJECT_SIZE);
    }

    // 4. Create Global Descriptor Set Layout (Set 0)
    bool isDawn = (device->GetPlatform() == rhi::RHIPlatform::Dawn);
    utl::vector<rhi::DescriptorSetLayoutBinding> globalBindings;
    {
        rhi::DescriptorSetLayoutBinding b;
        b.binding = FRAME_DATA_BINDING;
        b.descriptorType = rhi::DescriptorType::UniformBuffer;
        b.descriptorCount = 1;
        b.stageFlags = rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel;
        globalBindings.push_back(b);
    }
    {
        rhi::DescriptorSetLayoutBinding b;
        b.binding = LIGHT_DATA_BINDING;
        b.descriptorType = rhi::DescriptorType::UniformBuffer;
        b.descriptorCount = 1;
        b.stageFlags = rhi::ShaderStage::Pixel;
        globalBindings.push_back(b);
    }
    if (!isDawn) {
        {
            rhi::DescriptorSetLayoutBinding b;
            b.binding = SHADOW_MAP_BINDING;
            b.descriptorType = rhi::DescriptorType::CombinedImageSampler;
            b.descriptorCount = 1;
            b.stageFlags = rhi::ShaderStage::Pixel;
            globalBindings.push_back(b);
        }
        {
            rhi::DescriptorSetLayoutBinding b;
            b.binding = SHADOW_CUBE_MAP_BINDING;
            b.descriptorType = rhi::DescriptorType::CombinedImageSampler;
            b.descriptorCount = 1;
            b.stageFlags = rhi::ShaderStage::Pixel;
            globalBindings.push_back(b);
        }
    } else {
        // Dawn/WebGPU: separate texture + sampler bindings for shadow
        {
            rhi::DescriptorSetLayoutBinding b;
            b.binding = SHADOW_MAP_BINDING; // 13
            b.descriptorType = rhi::DescriptorType::SampledDepthImage;
            b.descriptorCount = 1;
            b.stageFlags = rhi::ShaderStage::Pixel;
            globalBindings.push_back(b);
        }
        {
            rhi::DescriptorSetLayoutBinding b;
            b.binding = SHADOW_CUBE_MAP_BINDING; // 14
            b.descriptorType = rhi::DescriptorType::Sampler;
            b.descriptorCount = 1;
            b.stageFlags = rhi::ShaderStage::Pixel;
            globalBindings.push_back(b);
        }
    }

    rhi::DescriptorSetLayoutDesc globalLayoutDesc;
    globalLayoutDesc.bindingCount = (u32)globalBindings.size();
    globalLayoutDesc.bindings = globalBindings.data();
    globalDescriptorSetLayout_ = device->CreateDescriptorSetLayout(globalLayoutDesc);

    // 5. Create Per-Object Descriptor Set Layout (Set 1)
    // Binding 0: PerObjectData (Dynamic Uniform Buffer)
    utl::vector<rhi::DescriptorSetLayoutBinding> perObjectBindings(1);
    perObjectBindings[0].binding = PER_OBJECT_BINDING; // Relative to Set 1
    perObjectBindings[0].descriptorType = rhi::DescriptorType::UniformBufferDynamic;
    perObjectBindings[0].descriptorCount = 1;
    perObjectBindings[0].stageFlags = rhi::ShaderStage::Vertex;

    rhi::DescriptorSetLayoutDesc perObjectLayoutDesc;
    perObjectLayoutDesc.bindingCount = 1;
    perObjectLayoutDesc.bindings = perObjectBindings.data();
    perObjectDescriptorSetLayout_ = device->CreateDescriptorSetLayout(perObjectLayoutDesc);

    // 6. Create Shadow Map Resources
    if (!isDawn) {
        // Shared Depth Buffer (Transient for Shadow Passes)
        rhi::TextureDesc shadowDepthDesc{
            { 2048, 2048, 1 },
            1,
            1,
            rhi::DataFormat::D32_Float,
            rhi::TextureType::Texture2D,
            rhi::TextureUsage::DepthStencil,
            rhi::GPUMemoryUsage::Static,
            "ShadowDepthBuffer"
        };

        shadowDepthBuffer_ = device->CreateTexture(shadowDepthDesc);
        if (shadowDepthBuffer_ == rhi::handles::INVALID_RESOURCE) return false;

        // Shadow Map Array (VSM Color Target: Depth, Depth^2)
        rhi::TextureDesc shadowMapDesc{
            { 2048, 2048, 1 },
            1,
            SHADOW_MAP_ARRAY_SIZE,
            rhi::DataFormat::RG32_Float,
            rhi::TextureType::Texture2DArray,
            rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess,
            rhi::GPUMemoryUsage::Static,
            "ShadowMapArray"
        };

        shadowMapArray_ = device->CreateTexture(shadowMapDesc);
        if (shadowMapArray_ == rhi::handles::INVALID_RESOURCE) return false;

        shadowMapDesc.name = "ShadowMapTempArray";
        shadowMapTempArray_ = device->CreateTexture(shadowMapDesc);
        if (shadowMapTempArray_ == rhi::handles::INVALID_RESOURCE) return false;

        rhi::TextureDesc shadowCubeMapDesc{
            { 1024, 1024, 1 },
            1,
            MAX_POINT_SHADOWS * 6,
            rhi::DataFormat::RG32_Float,
            rhi::TextureType::TextureCubeArray,
            rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource,
            rhi::GPUMemoryUsage::Static,
            "ShadowCubeMapArray"
        };

        shadowCubeMapArray_ = device->CreateTexture(shadowCubeMapDesc);
        if (shadowCubeMapArray_ == rhi::handles::INVALID_RESOURCE) return false;

        rhi::SamplerDesc shadowSamplerDesc{};
        shadowSamplerDesc.minFilter = rhi::FilterMode::Linear;
        shadowSamplerDesc.magFilter = rhi::FilterMode::Linear;
        shadowSamplerDesc.addressU = rhi::TextureAddressMode::Clamp;
        shadowSamplerDesc.addressV = rhi::TextureAddressMode::Clamp;
        shadowSamplerDesc.addressW = rhi::TextureAddressMode::Clamp;
        shadowSamplerDesc.borderColor = {1.0f, 1.0f, 1.0f, 1.0f};

        shadowMapSampler_ = device->CreateSampler(shadowSamplerDesc);
        shadowCubeMapSampler_ = device->CreateSampler(shadowSamplerDesc);
    } else {
        // Dawn: real shadow depth texture + sampler + pipeline
        rhi::TextureDesc shadowDepthDesc{
            { 2048, 2048, 1 },
            1,
            1,
            rhi::DataFormat::D32_Float,
            rhi::TextureType::Texture2D,
            rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource,
            rhi::GPUMemoryUsage::Static,
            "ShadowDepthBuffer"
        };
        shadowDepthBuffer_ = device->CreateTexture(shadowDepthDesc);

        rhi::SamplerDesc shadowSamplerDesc{};
        shadowSamplerDesc.minFilter = rhi::FilterMode::Linear;
        shadowSamplerDesc.magFilter = rhi::FilterMode::Linear;
        shadowSamplerDesc.addressU = rhi::TextureAddressMode::Clamp;
        shadowSamplerDesc.addressV = rhi::TextureAddressMode::Clamp;
        shadowSamplerDesc.addressW = rhi::TextureAddressMode::Clamp;
        shadowSamplerDesc.borderColor = {1.0f, 1.0f, 1.0f, 1.0f};
        shadowSamplerDesc.comparisonFunc = rhi::ComparisonFunc::Never;
        shadowMapSampler_ = device->CreateSampler(shadowSamplerDesc);

        // Shadow DSL: 1 binding — ShadowPerObject uniform buffer
        {
            rhi::DescriptorSetLayoutBinding bind{};
            bind.binding = 0;
            bind.descriptorType = rhi::DescriptorType::UniformBuffer;
            bind.descriptorCount = 1;
            bind.stageFlags = rhi::ShaderStage::Vertex;
            rhi::DescriptorSetLayoutDesc dslDesc;
            dslDesc.bindingCount = 1;
            dslDesc.bindings = &bind;
            dawnShadowDSL_ = device->CreateDescriptorSetLayout(dslDesc);

            rhi::PipelineLayoutDesc plDesc;
            plDesc.setLayoutCount = 1;
            plDesc.setLayouts = &dawnShadowDSL_;
            dawnShadowPipelineLayout_ = device->CreatePipelineLayout(plDesc);
        }

        // Shadow depth pipeline (vertex-only, front-face cull)
        {
            std::string shaderSrc;
#ifdef __EMSCRIPTEN__
            shaderSrc = dawn::LoadWGSL("ShadowDepth");
#else
            auto platform = device->GetPlatform();
            std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "ShadowDepth");
            std::ifstream file(shaderPath, std::ios::ate | std::ios::binary);
            if (!file.is_open()) {
                shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + shaderPath;
                file.open(shaderPath, std::ios::ate | std::ios::binary);
            }
            if (file.is_open()) {
                size_t size = file.tellg();
                utl::vector<char> buf;
                buf.resize(size + 1);
                file.seekg(0);
                file.read(buf.data(), size);
                buf[size] = 0;
                shaderSrc = std::string(buf.data(), size);
            }
#endif
            if (!shaderSrc.empty()) {
                rhi::ShaderHandle vs = device->CreateShader(shaderSrc.data(), shaderSrc.size(), rhi::ShaderStage::Vertex, "shadow_vs");
                if (vs != rhi::handles::INVALID_SHADER) {
                    rhi::GraphicsPipelineDesc pipeDesc;
                    pipeDesc.vertexShader = vs;
                    pipeDesc.layout = dawnShadowPipelineLayout_;
                    pipeDesc.topology = rhi::PrimitiveTopology::TriangleList;
                    pipeDesc.renderTargetCount = 0;
                    pipeDesc.depthStencilFormat = rhi::DataFormat::D32_Float;
                    pipeDesc.enableDepthTest = true;
                    pipeDesc.enableDepthWrite = true;
                    pipeDesc.depthFunc = rhi::ComparisonFunc::Less;
                    pipeDesc.cullMode = rhi::CullMode::Front;

                    utl::vector<rhi::VertexInputAttribute> attrs(5);
                    attrs[0] = {0, 0, rhi::DataFormat::RGB32_Float, 0};
                    attrs[1] = {1, 0, rhi::DataFormat::R32_UInt, 12};
                    attrs[2] = {2, 0, rhi::DataFormat::R32_UInt, 16};
                    attrs[3] = {3, 0, rhi::DataFormat::R32_UInt, 20};
                    attrs[4] = {4, 0, rhi::DataFormat::RG32_Float, 24};
                    pipeDesc.vertexAttributes = attrs;
                    utl::vector<rhi::VertexInputBinding> binds(1);
                    binds[0] = {0, 32, true};
                    pipeDesc.vertexBindings = binds;

                    dawnShadowPipeline_ = device->CreateGraphicsPipeline(pipeDesc);
                }
            }
        }

        // Shadow per-object buffer (128 bytes) + descriptor set
        {
            rhi::BufferDesc bufDesc{};
            bufDesc.size = 128;
            bufDesc.type = rhi::BufferType::Constant;
            bufDesc.usage = rhi::GPUMemoryUsage::Dynamic;
            bufDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
            dawnShadowPerObjectBuf_ = device->CreateBuffer(bufDesc);
            dawnShadowPerObjectMapped_ = device->MapBuffer(dawnShadowPerObjectBuf_, 0, 128);

            rhi::DescriptorSetDesc dsDesc;
            dsDesc.layout = dawnShadowDSL_;
            dawnShadowPerObjectSet_ = device->CreateDescriptorSet(dsDesc);
            rhi::DescriptorBufferInfo bufInfo;
            bufInfo.buffer = dawnShadowPerObjectBuf_;
            bufInfo.offset = 0;
            bufInfo.range = 128;
            rhi::WriteDescriptorSet write;
            write.dstSet = dawnShadowPerObjectSet_;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = rhi::DescriptorType::UniformBuffer;
            write.bufferInfo = &bufInfo;
            device->UpdateDescriptorSets(1, &write);
        }
    }

    // 7. Allocate and Update Descriptor Sets
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        // Global Set
        rhi::DescriptorSetDesc globalSetDesc;
        globalSetDesc.layout = globalDescriptorSetLayout_;
        globalDescriptorSets_[i] = device->CreateDescriptorSet(globalSetDesc);

        rhi::DescriptorBufferInfo frameInfo;
        frameInfo.buffer = frameBuffers_[i];
        frameInfo.offset = 0;
        frameInfo.range = sizeof(rhi::GlobalShaderData);

        rhi::DescriptorBufferInfo lightInfo;
        lightInfo.buffer = lightBuffers_[i];
        lightInfo.offset = 0;
        lightInfo.range = sizeof(rhi::ForwardLightBuffer);

        rhi::WriteDescriptorSet writeFrame;
        writeFrame.dstSet = globalDescriptorSets_[i];
        writeFrame.dstBinding = FRAME_DATA_BINDING;
        writeFrame.descriptorType = rhi::DescriptorType::UniformBuffer;
        writeFrame.descriptorCount = 1;
        writeFrame.bufferInfo = &frameInfo;

        rhi::WriteDescriptorSet writeLight;
        writeLight.dstSet = globalDescriptorSets_[i];
        writeLight.dstBinding = LIGHT_DATA_BINDING;
        writeLight.descriptorType = rhi::DescriptorType::UniformBuffer;
        writeLight.descriptorCount = 1;
        writeLight.bufferInfo = &lightInfo;

        utl::vector<rhi::WriteDescriptorSet> writes;
        writes.push_back(writeFrame);
        writes.push_back(writeLight);

        if (!isDawn) {
            rhi::DescriptorImageInfo shadowInfo;
            shadowInfo.sampler = shadowMapSampler_;
            shadowInfo.imageView = shadowMapArray_;
            shadowInfo.imageLayout = rhi::ResourceState::ShaderResource;

            rhi::WriteDescriptorSet writeShadow;
            writeShadow.dstSet = globalDescriptorSets_[i];
            writeShadow.dstBinding = SHADOW_MAP_BINDING;
            writeShadow.descriptorType = rhi::DescriptorType::CombinedImageSampler;
            writeShadow.descriptorCount = 1;
            writeShadow.imageInfo = &shadowInfo;
            writes.push_back(writeShadow);

            rhi::DescriptorImageInfo shadowCubeInfo;
            shadowCubeInfo.sampler = shadowCubeMapSampler_;
            shadowCubeInfo.imageView = shadowCubeMapArray_;
            shadowCubeInfo.imageLayout = rhi::ResourceState::ShaderResource;

            rhi::WriteDescriptorSet writeShadowCube;
            writeShadowCube.dstSet = globalDescriptorSets_[i];
            writeShadowCube.dstBinding = SHADOW_CUBE_MAP_BINDING;
            writeShadowCube.descriptorType = rhi::DescriptorType::CombinedImageSampler;
            writeShadowCube.descriptorCount = 1;
            writeShadowCube.imageInfo = &shadowCubeInfo;
            writes.push_back(writeShadowCube);
        } else {
            // Dawn: write separate shadow depth texture + sampler bindings
            rhi::DescriptorImageInfo shadowTexInfo;
            shadowTexInfo.imageView = shadowDepthBuffer_;
            shadowTexInfo.imageLayout = rhi::ResourceState::ShaderResource;

            rhi::WriteDescriptorSet writeShadowTex;
            writeShadowTex.dstSet = globalDescriptorSets_[i];
            writeShadowTex.dstBinding = SHADOW_MAP_BINDING;
            writeShadowTex.descriptorType = rhi::DescriptorType::SampledDepthImage;
            writeShadowTex.descriptorCount = 1;
            writeShadowTex.imageInfo = &shadowTexInfo;
            writes.push_back(writeShadowTex);

            rhi::DescriptorImageInfo shadowSampInfo;
            shadowSampInfo.sampler = shadowMapSampler_;

            rhi::WriteDescriptorSet writeShadowSamp;
            writeShadowSamp.dstSet = globalDescriptorSets_[i];
            writeShadowSamp.dstBinding = SHADOW_CUBE_MAP_BINDING;
            writeShadowSamp.descriptorType = rhi::DescriptorType::Sampler;
            writeShadowSamp.descriptorCount = 1;
            writeShadowSamp.imageInfo = &shadowSampInfo;
            writes.push_back(writeShadowSamp);
        }

        device->UpdateDescriptorSets((u32)writes.size(), writes.data());

        // Per-Object Set
        rhi::DescriptorSetDesc perObjectSetDesc;
        perObjectSetDesc.layout = perObjectDescriptorSetLayout_;
        perObjectDescriptorSets_[i] = device->CreateDescriptorSet(perObjectSetDesc);
        
        rhi::DescriptorBufferInfo perObjectInfo;
        perObjectInfo.buffer = perObjectBuffers_[i];
        perObjectInfo.offset = 0;
        perObjectInfo.range = sizeof(rhi::PerObjectData); // Range of one slot, or whole buffer? Usually window size.

        rhi::WriteDescriptorSet writePerObject;
        writePerObject.dstSet = perObjectDescriptorSets_[i];
        writePerObject.dstBinding = PER_OBJECT_BINDING;
        writePerObject.descriptorType = rhi::DescriptorType::UniformBufferDynamic;
        writePerObject.descriptorCount = 1;
        writePerObject.bufferInfo = &perObjectInfo;

        device->UpdateDescriptorSets(1, &writePerObject);
    }

    // 8. Initialize Passes
    if (!isDawn) {

    if (!blurPass_.Initialize(device_)) {
        if (!isDawn) {
            std::cerr << "ForwardRenderer: Failed to initialize BlurPass" << std::endl;
            return false;
        }
        std::cerr << "ForwardRenderer: BlurPass skipped (Dawn)" << std::endl;
    }

    // Initialize SSR Pass
    if (!ssrPass_.Initialize(device_)) {
            std::cerr << "ForwardRenderer: Failed to initialize SSRPass" << std::endl;
            return false;
    }
    } // !isDawn

#ifndef DISABLE_PARTICLE_SYSTEM
    // Initialize Particle Pass (Dawn skips — no WGSL particle shaders)
    if (!particlePass_.initialize(device_)) {
        if (!isDawn) {
            std::cerr << "ForwardRenderer: Failed to initialize ParticlePass" << std::endl;
            return false;
        }
        std::cerr << "ForwardRenderer: ParticlePass skipped (Dawn)" << std::endl;
    }
#endif

    // Initialize Scene Extraction System
    if (!sceneExtractionSystem_.Initialize(device_, 10000, 100000)) {
        std::cerr << "ForwardRenderer: Failed to initialize SceneExtractionSystem" << std::endl;
        return false;
    }

    // Initialize Composite Pipeline (SSR Blending) — Dawn skips (no SSR)
    if (!isDawn) {
        rhi::DescriptorSetLayoutBinding binding;
        binding.binding = 0;
        binding.descriptorType = rhi::DescriptorType::CombinedImageSampler;
        binding.descriptorCount = 1;
        binding.stageFlags = rhi::ShaderStage::Pixel;
        
        rhi::DescriptorSetLayoutDesc layoutDesc;
        layoutDesc.bindings = &binding;
        layoutDesc.bindingCount = 1;
        compositeDescriptorSetLayout_ = device_->CreateDescriptorSetLayout(layoutDesc);
        
        rhi::PipelineLayoutDesc plDesc;
        plDesc.setLayouts = &compositeDescriptorSetLayout_;
        plDesc.setLayoutCount = 1;
        compositePipelineLayout_ = device_->CreatePipelineLayout(plDesc);
        
        // Shader Loading (SSRComposite)
        auto platform = device_->GetPlatform();
        std::string shaderPath = utils::ShaderRegistry::GetShaderPath(platform, "SSRComposite");
        std::ifstream file(shaderPath, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
             shaderPath = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + shaderPath;
             file.open(shaderPath, std::ios::ate | std::ios::binary);
        }
        if (file.is_open()) {
            size_t size = file.tellg();
            utl::vector<char> buf;
            buf.resize(size + 1);
            file.seekg(0);
            file.read(buf.data(), size);
            buf[size] = 0;
            
            rhi::ShaderHandle vs = device_->CreateShader(buf.data(), size, rhi::ShaderStage::Vertex, "vertexMain");
            
            rhi::ShaderHandle fs = device_->CreateShader(buf.data(), size, rhi::ShaderStage::Pixel, "fragmentMain");
            
            if (vs != rhi::handles::INVALID_SHADER && fs != rhi::handles::INVALID_SHADER) {
                rhi::GraphicsPipelineDesc pDesc;
                pDesc.vertexShader = vs;
                pDesc.pixelShader = fs;
                pDesc.layout = compositePipelineLayout_;
                
                // Additive Blending
                pDesc.enableBlend = true;
                pDesc.srcColorBlendFactor = rhi::BlendFactor::One; // Add
                pDesc.dstColorBlendFactor = rhi::BlendFactor::One; // Add
                pDesc.colorBlendOp = rhi::BlendOp::Add;
                pDesc.srcAlphaBlendFactor = rhi::BlendFactor::One;
                pDesc.dstAlphaBlendFactor = rhi::BlendFactor::One;
                pDesc.alphaBlendOp = rhi::BlendOp::Add;
                
                // No Depth Test for fullscreen quad usually, or Always pass
                pDesc.enableDepthTest = false;
                pDesc.enableDepthWrite = false;
                
                pDesc.cullMode = rhi::CullMode::None;
                
                pDesc.renderTargetCount = 1;
                pDesc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm;
                
                compositePipeline_ = device_->CreateGraphicsPipeline(pDesc);
            }
        }
    } // !isDawn

    return true;
}

void ForwardRenderer::Shutdown() {
    blurPass_.Shutdown();
    ssrPass_.Shutdown();
#ifndef DISABLE_PARTICLE_SYSTEM
    particlePass_.shutdown();
#endif
    sceneExtractionSystem_.Shutdown();

    if (device_) {
        if (ssrOutput_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyTexture(ssrOutput_);
        }
        if (compositePipeline_ != rhi::handles::INVALID_PIPELINE) {
            device_->DestroyPipeline(compositePipeline_);
        }
        if (compositePipelineLayout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
            device_->DestroyPipelineLayout(compositePipelineLayout_);
        }
        if (compositeDescriptorSetLayout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            device_->DestroyDescriptorSetLayout(compositeDescriptorSetLayout_);
        }

        // Dawn shadow cleanup
        if (dawnShadowPipeline_ != rhi::handles::INVALID_PIPELINE) {
            device_->DestroyPipeline(dawnShadowPipeline_);
        }
        if (dawnShadowPipelineLayout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
            device_->DestroyPipelineLayout(dawnShadowPipelineLayout_);
        }
        if (dawnShadowDSL_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyDescriptorSetLayout(dawnShadowDSL_);
        }
        if (dawnShadowPerObjectBuf_ != rhi::handles::INVALID_RESOURCE) {
            if (dawnShadowPerObjectMapped_) device_->UnmapBuffer(dawnShadowPerObjectBuf_);
            device_->DestroyBuffer(dawnShadowPerObjectBuf_);
        }

        for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
            // Buffers
            if (lightBuffers_[i] != rhi::handles::INVALID_RESOURCE) {
                device_->UnmapBuffer(lightBuffers_[i]);
                device_->DestroyBuffer(lightBuffers_[i]);
            }
            if (frameBuffers_[i] != rhi::handles::INVALID_RESOURCE) {
                device_->UnmapBuffer(frameBuffers_[i]);
                device_->DestroyBuffer(frameBuffers_[i]);
            }
            if (perObjectBuffers_[i] != rhi::handles::INVALID_RESOURCE) {
                device_->UnmapBuffer(perObjectBuffers_[i]);
                device_->DestroyBuffer(perObjectBuffers_[i]);
            }

            // Sets
            // (Sets are usually freed with pool, but if individual destroy is supported...)
        }
        
        if (globalDescriptorSetLayout_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyDescriptorSetLayout(globalDescriptorSetLayout_);
        }
        if (perObjectDescriptorSetLayout_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyDescriptorSetLayout(perObjectDescriptorSetLayout_);
        }
        
        if (shadowMapArray_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyTexture(shadowMapArray_);
        }
        if (shadowMapSampler_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroySampler(shadowMapSampler_);
        }
        if (shadowDepthBuffer_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyTexture(shadowDepthBuffer_);
        }
        if (shadowCubeMapArray_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyTexture(shadowCubeMapArray_);
        }
        if (shadowCubeMapSampler_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroySampler(shadowCubeMapSampler_);
        }
    }
    device_ = nullptr;
}

void ForwardRenderer::RenderReflections(rhi::RHICommandBuffer* cmdBuffer,
                                        const RenderScene& scene,
                                        const RenderView& mainView,
                                        const std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>>& materials,
                                        u32 frameIndex) {
    const auto& planes = scene.GetReflectionPlanes();
    if (planes.empty()) return;

    if (reflectionSampler_ == rhi::handles::INVALID_RESOURCE) {
        rhi::SamplerDesc desc{};
        desc.minFilter = rhi::FilterMode::Linear;
        desc.magFilter = rhi::FilterMode::Linear;
        desc.addressU = rhi::TextureAddressMode::Clamp;
        desc.addressV = rhi::TextureAddressMode::Clamp;
        reflectionSampler_ = device_->CreateSampler(desc);
    }

    for (const auto& plane : planes) {
        ReflectionResource& res = reflectionResources_[plane.entityId];
        
        // 1. Create Resources if needed
        if (res.texture == rhi::handles::INVALID_RESOURCE) {
             rhi::TextureDesc desc{
                 { 1024, 1024, 1 },
                 1,
                 1,
                 rhi::DataFormat::BGRA8_UNorm,
                 rhi::TextureType::Texture2D,
                 rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource,
                 rhi::GPUMemoryUsage::Static,
                 "ReflectionTexture"
             };
             res.texture = device_->CreateTexture(desc);

             desc.format = rhi::DataFormat::D32_Float;
             desc.usage = rhi::TextureUsage::DepthStencil;
             desc.name = "ReflectionDepth";
             res.depth = device_->CreateTexture(desc);
        }

        // 2. Calculate Reflection Matrix
        rhi::math::v3 N = plane.normal;
        rhi::math::v3 P = plane.position;
        float d = -rhi::math::dot(N, P);
        
        float nx = N.x; float ny = N.y; float nz = N.z;
        rhi::math::m4x4 reflectionMat;
        reflectionMat.columns[0] = {1.0f - 2.0f*nx*nx, -2.0f*ny*nx, -2.0f*nz*nx, 0.0f};
        reflectionMat.columns[1] = {-2.0f*nx*ny, 1.0f - 2.0f*ny*ny, -2.0f*nz*ny, 0.0f};
        reflectionMat.columns[2] = {-2.0f*nx*nz, -2.0f*ny*nz, 1.0f - 2.0f*nz*nz, 0.0f};
        reflectionMat.columns[3] = {-2.0f*nx*d, -2.0f*ny*d, -2.0f*nz*d, 1.0f};

        rhi::math::m4x4 mainViewMat = mainView.GetViewMatrix();
        rhi::math::m4x4 reflectionViewMat = mainViewMat * reflectionMat;
        
        // Create View
        RenderView reflectionView;
        reflectionView.SetViewMatrix(reflectionViewMat);
        reflectionView.SetProjectionMatrix(mainView.GetProjectionMatrix());
        
        rhi::ViewportDesc viewport;
        viewport.topLeft = {0.0f, 0.0f};
        viewport.size = {1024.0f, 1024.0f};
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        reflectionView.SetViewport(viewport);
        
        rhi::Rect scissor;
        scissor.offset = {0, 0};
        scissor.extent = {1024, 1024};
        reflectionView.SetScissor(scissor);
        
        // Cull Scene
        reflectionView.Cull(scene);
        
        // 3. Prepare Global Data
        if (res.frameBuffer == rhi::handles::INVALID_RESOURCE) {
            rhi::BufferDesc bDesc{};
            bDesc.size = sizeof(rhi::GlobalShaderData);
            bDesc.type = rhi::BufferType::Constant;
            bDesc.usage = rhi::GPUMemoryUsage::Dynamic;
            bDesc.bindFlags = static_cast<u32>(rhi::ResourceUsage::ConstantBuffer);
            res.frameBuffer = device_->CreateBuffer(bDesc);
            res.frameBufferMapped = device_->MapBuffer(res.frameBuffer);
            
            rhi::DescriptorSetDesc dsDesc;
            dsDesc.layout = globalDescriptorSetLayout_;
            res.descriptorSet = device_->CreateDescriptorSet(dsDesc);
            
            // Bind Buffer
            rhi::DescriptorBufferInfo bufferInfo;
            bufferInfo.buffer = res.frameBuffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(rhi::GlobalShaderData);

            rhi::WriteDescriptorSet update;
            update.dstSet = res.descriptorSet;
            update.dstBinding = FRAME_DATA_BINDING;
            update.descriptorType = rhi::DescriptorType::UniformBuffer;
            update.descriptorCount = 1;
            update.bufferInfo = &bufferInfo;
            
            // Bind Lights (reuse main light buffer)
            rhi::DescriptorBufferInfo lightBufferInfo;
            lightBufferInfo.buffer = lightBuffers_[frameIndex];
            lightBufferInfo.offset = 0;
            lightBufferInfo.range = sizeof(rhi::ForwardLightBuffer);

            rhi::WriteDescriptorSet lightUpdate;
            lightUpdate.dstSet = res.descriptorSet;
            lightUpdate.dstBinding = LIGHT_DATA_BINDING;
            lightUpdate.descriptorType = rhi::DescriptorType::UniformBuffer;
            lightUpdate.descriptorCount = 1;
            lightUpdate.bufferInfo = &lightBufferInfo;
            
            // Bind Shadows (reuse main shadow maps)
            rhi::DescriptorImageInfo shadowInfo;
            shadowInfo.imageView = shadowMapArray_;
            shadowInfo.sampler = shadowMapSampler_;
            shadowInfo.imageLayout = rhi::ResourceState::ShaderResource;

            rhi::WriteDescriptorSet shadowUpdate;
            shadowUpdate.dstSet = res.descriptorSet;
            shadowUpdate.dstBinding = SHADOW_MAP_BINDING;
            shadowUpdate.descriptorType = rhi::DescriptorType::CombinedImageSampler;
            shadowUpdate.descriptorCount = 1;
            shadowUpdate.imageInfo = &shadowInfo;
            
            rhi::DescriptorImageInfo shadowCubeInfo;
            shadowCubeInfo.imageView = shadowCubeMapArray_;
            shadowCubeInfo.sampler = shadowCubeMapSampler_;
            shadowCubeInfo.imageLayout = rhi::ResourceState::ShaderResource;

            rhi::WriteDescriptorSet shadowCubeUpdate;
            shadowCubeUpdate.dstSet = res.descriptorSet;
            shadowCubeUpdate.dstBinding = SHADOW_CUBE_MAP_BINDING;
            shadowCubeUpdate.descriptorType = rhi::DescriptorType::CombinedImageSampler;
            shadowCubeUpdate.descriptorCount = 1;
            shadowCubeUpdate.imageInfo = &shadowCubeInfo;
            
            rhi::WriteDescriptorSet updates[] = {update, lightUpdate, shadowUpdate, shadowCubeUpdate};
            device_->UpdateDescriptorSets(4, updates);
        }
        
        // Update Frame Buffer Content
        if (res.frameBufferMapped && frameBuffersMapped_[frameIndex]) {
             rhi::GlobalShaderData* data = static_cast<rhi::GlobalShaderData*>(res.frameBufferMapped);
             rhi::GlobalShaderData* mainData = static_cast<rhi::GlobalShaderData*>(frameBuffersMapped_[frameIndex]);
             *data = *mainData; // Copy lights etc
             
             data->view = reflectionView.GetViewMatrix();
             data->projection = reflectionView.GetProjectionMatrix();
             data->viewProjection = reflectionView.GetViewProjectionMatrix();
             
             rhi::math::m4x4 viewInv = rhi::math::Inverse(data->view);
             rhi::math::v3 cameraPos = {viewInv.columns[3][0], viewInv.columns[3][1], viewInv.columns[3][2]};
             rhi::math::v3 cameraDir = {viewInv.columns[2][0], viewInv.columns[2][1], viewInv.columns[2][2]};
             data->cameraPositionAndViewWidth = {cameraPos.x, cameraPos.y, cameraPos.z, 1024.0f};
             data->cameraDirectionAndViewHeight = {cameraDir.x, cameraDir.y, cameraDir.z, 1024.0f};
        }

        // 4. Render Pass
        rhi::RenderPassDesc passDesc{};
        passDesc.colorAttachments.resize(1);
        passDesc.colorAttachments[0].texture = res.texture;
        passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear;
        passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;
        passDesc.colorAttachments[0].clearValue = rhi::ClearValue{ math::v4{ 0.1f, 0.1f, 0.1f, 1.0f } };
        
        passDesc.depthAttachment.texture = res.depth;
        passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
        passDesc.depthAttachment.storeOp = rhi::StoreAction::DontCare;
        passDesc.depthAttachment.clearValue = rhi::ClearValue{ math::v4{ 1.0f, 0.0f, 0.0f, 1.0f } };
        
        cmdBuffer->BeginRenderPass(passDesc);
        cmdBuffer->SetViewport(viewport);
        cmdBuffer->SetScissor(scissor);
        
        OpaquePass(cmdBuffer, reflectionView, materials, reflectionView.GetVisibleProxies(), frameIndex, false, res.descriptorSet, PipelineFlags::Reflection);
                
                cmdBuffer->EndRenderPass();

                // Transition Reflection Texture to ShaderResource for sampling in Main Pass
                rhi::ResourceBarrier barrier;
                barrier.resource = res.texture;
                barrier.beforeState = rhi::ResourceState::RenderTarget;
                barrier.afterState = rhi::ResourceState::ShaderResource;
                barrier.subresource = rhi::RHI_ALL_SUBRESOURCES;
                cmdBuffer->InsertBarrier(&barrier, 1);
                
                // 5. Update Material
        for (const auto& proxy : scene.GetProxies()) {
             if (proxy.entityId == plane.entityId) {
                 auto it = materials.find(proxy.materialId);
                 if (it != materials.end()) {
                     it->second->SetTexture(2, res.texture); // Binding 2
                     it->second->SetSampler(2, reflectionSampler_);
                     it->second->Update(device_);
                 }
                 break;
             }
        }
    }
}

void ForwardRenderer::RenderDawnShadowPass(rhi::RHICommandBuffer* cmdBuffer,
                                           const RenderScene& scene,
                                           const RenderView& view,
                                           const std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>>& /*materials*/) {
    if (dawnShadowPipeline_ == rhi::handles::INVALID_PIPELINE) return;
    if (shadowDepthBuffer_ == rhi::handles::INVALID_RESOURCE) return;

    // Compute light direction from first directional light
    rhi::math::v3 lightDir = {0.0f, -1.0f, 0.0f};
    const auto& allLights = scene.GetLights();
    for (size_t i = 0; i < allLights.size(); ++i) {
        if (allLights[i].type == LightType::Directional) {
            lightDir = allLights[i].direction;
            break;
        }
    }
    float len = sqrtf(lightDir.x * lightDir.x + lightDir.y * lightDir.y + lightDir.z * lightDir.z);
    if (len > 0.0001f) lightDir = lightDir / len;

    rhi::math::v3 up{0.0f, 1.0f, 0.0f};
    if (fabsf(lightDir.y) > 0.99f) up = {1.0f, 0.0f, 0.0f};

    // Fixed ortho size — constant snap grid ensures shadow stability
    constexpr float orthoHalf = 40.0f;
    constexpr float shadowMapSize = 2048.0f;
    constexpr float texelSize = (2.0f * orthoHalf) / shadowMapSize;

    // Compute camera frustum center for light view positioning
    rhi::math::m4x4 invVP = rhi::math::Inverse(view.GetViewProjectionMatrix());
    rhi::math::v3 corners[8];
    for (int i = 0; i < 8; i++) {
        float x = (i & 1) ? 1.0f : -1.0f;
        float y = (i & 2) ? 1.0f : -1.0f;
        float z = (i >= 4) ? 1.0f : 0.0f;
        rhi::math::v4 c = invVP * rhi::math::v4{x, y, z, 1.0f};
        corners[i] = {c.x / c.w, c.y / c.w, c.z / c.w};
    }
    rhi::math::v3 center{0, 0, 0};
    for (int i = 0; i < 8; i++) {
        center.x += corners[i].x; center.y += corners[i].y; center.z += corners[i].z;
    }
    center.x /= 8.0f; center.y /= 8.0f; center.z /= 8.0f;

    // Light view from frustum center (rotation is constant — depends only on lightDir)
    rhi::math::v3 lightEye = center - lightDir * 50.0f;
    rhi::math::m4x4 lightView = rhi::math::CreateLookAtMatrix(lightEye, center, up);

    // Snap center in light space to texel boundaries (constant grid)
    rhi::math::v4 centerLS = lightView * rhi::math::v4{center.x, center.y, center.z, 1.0f};
    float snappedX = roundf(centerLS.x / texelSize) * texelSize;
    float snappedY = roundf(centerLS.y / texelSize) * texelSize;
    lightView.columns[3][0] += (snappedX - centerLS.x);
    lightView.columns[3][1] += (snappedY - centerLS.y);

    // Z bounds from frustum (only depth range varies)
    float minZ = 1e9f, maxZ = -1e9f;
    for (int i = 0; i < 8; i++) {
        rhi::math::v4 ls = lightView * rhi::math::v4{corners[i].x, corners[i].y, corners[i].z, 1.0f};
        minZ = fminf(minZ, ls.z);
        maxZ = fmaxf(maxZ, ls.z);
    }

    rhi::math::m4x4 lightProj = rhi::math::CreateOrthographicMatrix(
        -orthoHalf, orthoHalf, -orthoHalf, orthoHalf, minZ, maxZ);

    dawnShadowLightVP_ = lightProj * lightView;

    // Begin depth-only render pass
    rhi::RenderPassDesc passDesc{};
    passDesc.depthAttachment.texture = shadowDepthBuffer_;
    passDesc.depthAttachment.format = rhi::DataFormat::D32_Float;
    passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
    passDesc.depthAttachment.clearValue.depth = 1.0f;
    passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;

    rhi::ViewportDesc vp;
    vp.size = {2048.0f, 2048.0f};
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;

    cmdBuffer->BeginRenderPass(passDesc);
    cmdBuffer->SetViewport(vp);
    cmdBuffer->SetScissor({{0, 0}, {2048, 2048}});
    cmdBuffer->BindGraphicsPipeline(dawnShadowPipeline_);

    if (dawnShadowPerObjectSet_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, dawnShadowPipelineLayout_,
                                      0, 1, &dawnShadowPerObjectSet_, 0, nullptr);
    }

    // Draw all visible proxies
    const auto& proxies = view.GetVisibleProxies();
    for (const auto* proxy : proxies) {
        if (!proxy) continue;

        if (dawnShadowPerObjectMapped_) {
            auto* p = static_cast<float*>(dawnShadowPerObjectMapped_);
            // world matrix (64 bytes)
            memcpy(p, &proxy->transform, 64);
            // worldLightVP (64 bytes)
            rhi::math::m4x4 worldLightVP = dawnShadowLightVP_ * proxy->transform;
            memcpy(p + 16, &worldLightVP, 64);
            device_->SetBufferDirtySize(dawnShadowPerObjectBuf_, 128);
        }

        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->meshId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }
    }

    cmdBuffer->EndRenderPass();

    // Barrier: depth texture → ShaderResource for forward pass sampling
    rhi::ResourceBarrier b{};
    b.resource = shadowDepthBuffer_;
    b.beforeState = rhi::ResourceState::DepthStencil;
    b.afterState = rhi::ResourceState::ShaderResource;
    b.subresource = 0xFFFFFFFF;
    cmdBuffer->InsertBarrier(&b, 1);
}

void ForwardRenderer::ShadowPass(rhi::RHICommandBuffer* cmdBuffer,
                                 const RenderView& view,
                                 rhi::ResourceHandle shadowMap,
                                 const std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>>& materials,
                                 const utl::vector<const RenderProxy*>& proxies,
                                 u32 frameIndex,
                                 u32 cascadeIndex) {
    // Ensure we run the pass to Clear the texture even if no proxies are visible
    // if (proxies.empty()) return;

    rhi::RenderPassDesc passDesc{};
    
    // VSM Shadow Pass: Output (Depth, Depth^2) to Color Attachment
    // Use shared depth buffer for Z-Test
    passDesc.colorAttachments.resize(1);
    passDesc.colorAttachments[0].texture = shadowMap;
    passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear; 
    passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;
    passDesc.colorAttachments[0].clearValue = rhi::ClearValue{ math::v4{ 1.0f, 1.0f, 1.0f, 1.0f } };
    passDesc.colorAttachments[0].arrayLayer = static_cast<u16>(cascadeIndex);

    passDesc.depthAttachment.texture = shadowDepthBuffer_;
    passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear; 
    passDesc.depthAttachment.storeOp = rhi::StoreAction::DontCare;
    passDesc.depthAttachment.clearValue = rhi::ClearValue{ math::v4{ 1.0f, 0.0f, 0.0f, 1.0f } };
    passDesc.depthAttachment.arrayLayer = 0;

    cmdBuffer->BeginRenderPass(passDesc);

    // Set Viewport and Scissor from View
    cmdBuffer->SetViewport(view.GetViewport());
    cmdBuffer->SetScissor(view.GetScissor());

    // Bind Global Set (Set 0)
    cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, 
                                  materials.empty() ? rhi::handles::INVALID_PIPELINE_LAYOUT : materials.begin()->second->GetMaterial()->GetPipelineLayout(), 
                                  0, 1, &globalDescriptorSets_[frameIndex], 0, nullptr);

    for (const auto* proxy : proxies) {
        auto it = materials.find(proxy->materialId);
        if (it == materials.end() || !it->second) continue;
        MaterialInstance* mi = it->second.get();
        Material* mat = mi->GetMaterial();
        
        // Shadow Pipeline (Depth Only + Shadow Flag)
        rhi::PipelineHandle pipeline = mat->GetPipeline(device_, rhi::handles::INVALID_RESOURCE, 0, PipelineFlags::Shadow);
        cmdBuffer->BindGraphicsPipeline(pipeline);

        // Update Per-Object Data for Shadow View
        u32 alignedSize = (sizeof(rhi::PerObjectData) + 255) & ~255;
        if (perObjectBufferOffset_ + alignedSize > MAX_PER_OBJECT_SIZE) break;

        auto* perObjectData = reinterpret_cast<rhi::PerObjectData*>(
            static_cast<u8*>(perObjectBuffersMapped_[frameIndex]) + perObjectBufferOffset_);
        
        perObjectData->world = proxy->transform;
        perObjectData->invWorld = rhi::math::Inverse(proxy->transform);
        perObjectData->worldViewProjection = view.GetViewProjectionMatrix() * proxy->transform;

        // Bind Per-Object Set (Set 1) with Dynamic Offset
        u32 dynamicOffset = perObjectBufferOffset_;
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 1, 1, &perObjectDescriptorSets_[frameIndex], 1, &dynamicOffset);

        // Draw
        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->meshId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }

        perObjectBufferOffset_ += alignedSize;
    }

    cmdBuffer->EndRenderPass();
}


void ForwardRenderer::SetupLights(const RenderScene& scene, 
                                  u32 frameIndex, 
                                  rhi::GlobalShaderData* globalData,
                                  const utl::vector<RenderView>& shadowViews,
                                  const utl::vector<float>& splits,
                                  const std::unordered_map<u32, int>& lightShadowIndices,
                                  const std::unordered_map<u32, rhi::math::m4x4>& lightViewProjs) {
    if (frameIndex >= rhi::MAX_FRAMES_IN_FLIGHT || !lightBuffersMapped_[frameIndex]) return;

    auto* buffer = static_cast<rhi::ForwardLightBuffer*>(lightBuffersMapped_[frameIndex]);
    buffer->directionalLightCount = 0;
    buffer->punctualLightCount = 0;

    const auto& allLights = scene.GetLights();
    for (size_t i = 0; i < allLights.size(); ++i) {
        const auto& light = allLights[i];
        if (light.type == LightType::Directional) {
            if (buffer->directionalLightCount < 4) {
                auto& dl = buffer->directionalLights[buffer->directionalLightCount++];

                if (!shadowViews.empty()) {
                    for (size_t j = 0; j < std::min<size_t>(shadowViews.size(), 4); ++j) {
                        dl.viewProjections[j] = shadowViews[j].GetViewProjectionMatrix();
                    }
                }

                // Dawn: fill viewProjections[0] with externally-provided shadow light VP
                bool isDawnLight = (device_->GetPlatform() == rhi::RHIPlatform::Dawn);
                if (isDawnLight && shadowDepthBuffer_ != rhi::handles::INVALID_RESOURCE) {
                    dl.viewProjections[0] = dawnShadowLightVP_;
                }
                
                if (!splits.empty()) {
                    dl.splits = {
                        splits.size() > 0 ? splits[0] : 0.0f,
                        splits.size() > 1 ? splits[1] : 0.0f,
                        splits.size() > 2 ? splits[2] : 0.0f,
                        splits.size() > 3 ? splits[3] : 0.0f
                    };
                }

                dl.directionAndIntensity = {light.direction.x, light.direction.y, light.direction.z, light.intensity};
                dl.colorAndShadow = {light.color.x, light.color.y, light.color.z, 1.0f}; // Shadow Enabled
            }
        } else {
            if (buffer->punctualLightCount < 128) {
                auto& pl = buffer->lights[buffer->punctualLightCount++];
                pl.position = light.position;
                pl.intensity = light.intensity;
                pl.direction = light.direction;
                pl.range = light.range;
                pl.color = light.color;
                pl.cosUmbra = light.outerCone;
                pl.cosPenumbra = light.innerCone;
                pl.attenuation = {1.0f, 0.0f, 0.0f}; // Default attenuation
                
                if (light.type == LightType::Point) pl.lightType = 1;
                else if (light.type == LightType::Spot) pl.lightType = 2;
                else pl.lightType = 0;

                // Check shadow index
                auto it = lightShadowIndices.find(static_cast<u32>(i));
                pl.shadowIndex = (it != lightShadowIndices.end()) ? it->second : -1;

                // Check view projection (for Spot Lights mainly)
                auto itVP = lightViewProjs.find(static_cast<u32>(i));
                if (itVP != lightViewProjs.end()) {
                    pl.viewProjection = itVP->second;
                } else {
                    pl.viewProjection = rhi::math::MatrixIdentity();
                }
            }
        }
    }

    if (globalData) {
        globalData->numDirectionalLights = buffer->directionalLightCount;
        globalData->numPunctualLights = buffer->punctualLightCount;
    }
}

void ForwardRenderer::Render(rhi::RHICommandBuffer* cmdBuffer, 
                             const RenderScene& scene, 
                             const RenderView& view, 
                             rhi::ResourceHandle renderTarget, 
                             rhi::ResourceHandle depthStencil,
                             const ::std::unordered_map<id::id_type, ::std::shared_ptr<MaterialInstance>>& materials,
                             u32 frameIndex,
                             u32 width,
                             u32 height) {
    if (!device_ || !cmdBuffer) return;
    (void)scene;

    // Reset Per-Object Buffer Offset
    perObjectBufferOffset_ = 0;

    // Scene extraction requires valid ECS entities (transform system). Skip on Dawn
    // where we create proxies directly without the ECS.
    bool skipSceneExtraction = (device_->GetPlatform() == rhi::RHIPlatform::Dawn);

    if (sceneExtractionEnabled_ && !skipSceneExtraction) {
        sceneExtractionSystem_.QueryDirtyTransforms(scene);

        if (sceneExtractionSystem_.NeedsFullRebuild()) {
            sceneExtractionSystem_.ExtractScene(scene);
        } else {
            if (!sceneExtractionSystem_.ExtractScene(scene)) {
                std::cerr << "ForwardRenderer: Partial scene update failed" << std::endl;
            }
        }
    }

    // === Shadow Pass (Before Main Pass) ===
    utl::vector<RenderView> csmViews;
    utl::vector<float> cascadeSplits;
    ::std::unordered_map<u32, int> lightShadowIndices;
    ::std::unordered_map<u32, rhi::math::m4x4> lightViewProjs;

    // Process Lights for Shadows
    const auto& allLights = scene.GetLights();
    rhi::math::v3 lightDir = {0, -1, 0};
    bool hasDirectionalLight = false;
    u32 spotShadowCount = 0;
    u32 pointShadowCount = 0;

    // Stage 1: Dawn uses its own depth-only shadow pass; Metal/Vulkan uses VSM shadow loop
    bool skipShadows = (device_->GetPlatform() == rhi::RHIPlatform::Dawn);

    if (skipShadows) {
        RenderDawnShadowPass(cmdBuffer, scene, view, materials);
    } else {
        for (size_t i = 0; i < allLights.size(); ++i) {
            const auto& light = allLights[i];

        if (light.type == LightType::Directional) {
            if (!hasDirectionalLight && shadowMapArray_ != rhi::handles::INVALID_RESOURCE) {
                lightDir = light.direction;
                hasDirectionalLight = true;
                
                utils::CascadeConfig config;
                config.shadowMapSize = 2048; 
                config.splitLambda = 0.5f;
                config.cascadeCount = MAX_CSM_CASCADES;
                
                utils::CalculateCascadeSplits(config, cascadeSplits);
                utils::CreateCascadeViews(view, lightDir, config, csmViews);

                for (u32 j = 0; j < csmViews.size(); ++j) {
                    auto& shadowView = csmViews[j];
                    shadowView.Cull(scene);
                    ShadowPass(cmdBuffer, shadowView, shadowMapArray_, materials, shadowView.GetVisibleProxies(), frameIndex, j);
                }
            }
        } 
        else if (light.type == LightType::Spot) {
            if (spotShadowCount < MAX_SPOT_SHADOWS && shadowMapArray_ != rhi::handles::INVALID_RESOURCE) {
                 u32 shadowIndex = MAX_CSM_CASCADES + spotShadowCount;
                 RenderView shadowView;
                 utils::CreateSpotShadowView(light.position, light.direction, light.outerCone, light.range, 2048, shadowView);
                 shadowView.Cull(scene);
                 
                 ShadowPass(cmdBuffer, shadowView, shadowMapArray_, materials, shadowView.GetVisibleProxies(), frameIndex, shadowIndex);
                 
                 lightShadowIndices[static_cast<u32>(i)] = shadowIndex;
                 lightViewProjs[static_cast<u32>(i)] = shadowView.GetViewProjectionMatrix();
                 spotShadowCount++;
            }
        }
        else if (light.type == LightType::Point) {
            if (pointShadowCount < MAX_POINT_SHADOWS && shadowCubeMapArray_ != rhi::handles::INVALID_RESOURCE) {
                u32 baseSlice = pointShadowCount * 6;
                utl::vector<RenderView> pointViews;
                utils::CreatePointShadowViews(light.position, light.range, 1024, pointViews);
                
                for(int face = 0; face < 6; ++face) {
                    auto& shadowView = pointViews[face];
                    shadowView.Cull(scene);
                    ShadowPass(cmdBuffer, shadowView, shadowCubeMapArray_, materials, shadowView.GetVisibleProxies(), frameIndex, baseSlice + face);
                }
                
                lightShadowIndices[static_cast<u32>(i)] = pointShadowCount;
                pointShadowCount++;
            }
        }
    } // end for loop (non-Dawn shadows)
    } // end else (non-Dawn shadow path)

    // Execute Blur Pass for VSM (ShadowMapArray -> Temp -> ShadowMapArray)
    if (!skipShadows && shadowMapArray_ != rhi::handles::INVALID_RESOURCE && shadowMapTempArray_ != rhi::handles::INVALID_RESOURCE) {
        // Ensure ShadowMapArray is in ShaderResource state (it was RenderTarget)
        // Note: The RenderPass end automatically transitions attachments to ShaderResource? 
        // Metal usually handles this, but for explicit barriers in Vulkan style RHI:
        
        rhi::ResourceBarrier barrier;
        barrier.resource = shadowMapArray_;
        barrier.beforeState = rhi::ResourceState::RenderTarget; // It was written to
        barrier.afterState = rhi::ResourceState::ShaderResource; // Read in Compute
        barrier.subresource = rhi::RHI_ALL_SUBRESOURCES;
        cmdBuffer->InsertBarrier(&barrier, 1);

        // Execute Blur
        // VSM usually needs a small blur radius to smooth out moments
        blurPass_.Execute(cmdBuffer, 
                          shadowMapArray_,      // Input
                          shadowMapArray_,      // Output (Ping-Pong via Temp inside)
                          shadowMapTempArray_,  // Temp
                          2048, 2048,           // Size
                          SHADOW_MAP_ARRAY_SIZE,// Layers
                          frameIndex,
                          3, 1.0f);             // Radius 3, Sigma 1.0
        
        // Barrier for Pixel Shader Read (already handled at end of BlurPass? No, BlurPass ends with Output as ShaderResource)
        // BlurPass leaves Output in ShaderResource state, so we are good for Main Pass sampling.
    }

    // Render Planar Reflections (uses Shadow Maps)
    if (!skipShadows) {
        RenderReflections(cmdBuffer, scene, view, materials, frameIndex);
    }

    // 0. Update Frame Data
    if (frameIndex < rhi::MAX_FRAMES_IN_FLIGHT && frameBuffersMapped_[frameIndex]) {
        rhi::GlobalShaderData* frameData = static_cast<rhi::GlobalShaderData*>(frameBuffersMapped_[frameIndex]);
        frameData->view = view.GetViewMatrix();
        frameData->projection = view.GetProjectionMatrix();
        frameData->viewProjection = view.GetViewProjectionMatrix();

        rhi::math::m4x4 viewInv = rhi::math::Inverse(view.GetViewMatrix());
        rhi::math::v3 cameraPos = {viewInv.columns[3][0], viewInv.columns[3][1], viewInv.columns[3][2]};
        rhi::math::v3 cameraDir = {viewInv.columns[2][0], viewInv.columns[2][1], viewInv.columns[2][2]};
        frameData->deltaTime = deltaTime_;
        frameData->frameCount = static_cast<float>(frameNumber_);
        frameData->cameraPositionAndViewWidth = {cameraPos.x, cameraPos.y, cameraPos.z, static_cast<float>(width)};
        frameData->cameraDirectionAndViewHeight = {cameraDir.x, cameraDir.y, cameraDir.z, static_cast<float>(height)};

        SetupLights(scene, frameIndex, frameData, csmViews, cascadeSplits, lightShadowIndices, lightViewProjs);

    }

    // 1. Filter and Sort Proxies
    utl::vector<const RenderProxy*> opaqueProxies;
    utl::vector<const RenderProxy*> transparentProxies;
    opaqueProxies.reserve(view.GetVisibleProxies().size());
    transparentProxies.reserve(view.GetVisibleProxies().size());

    for (const auto* proxy : view.GetVisibleProxies()) {
        if (!proxy) continue;
        auto it = materials.find(proxy->materialId);
        if (it != materials.end() && it->second) {
            Material* mat = it->second->GetMaterial();
            if (mat) {
                if (mat->GetBlendState().enableBlend) {
                    transparentProxies.push_back(proxy);
                } else {
                    opaqueProxies.push_back(proxy);
                }
            }
        }
    }

    rhi::math::m4x4 viewInv = rhi::math::Inverse(view.GetViewMatrix());
    rhi::math::v3 cameraPos = {viewInv.columns[3][0], viewInv.columns[3][1], viewInv.columns[3][2]};

    // Sort Opaque (Front-to-Back)
    std::sort(opaqueProxies.begin(), opaqueProxies.end(), [&](const RenderProxy* a, const RenderProxy* b) {
        float distA = rhi::math::LengthSquared(a->worldAABB.Center() - cameraPos);
        float distB = rhi::math::LengthSquared(b->worldAABB.Center() - cameraPos);
        return distA < distB;
    });

    // Sort Transparent (Back-to-Front)
    std::sort(transparentProxies.begin(), transparentProxies.end(), [&](const RenderProxy* a, const RenderProxy* b) {
        float distA = rhi::math::LengthSquared(a->worldAABB.Center() - cameraPos);
        float distB = rhi::math::LengthSquared(b->worldAABB.Center() - cameraPos);
        return distA > distB;
    });

    // 2. Z-Prepass (Depth Only) — skip on Dawn (depth-only pipeline not supported)
    if (depthStencil != rhi::handles::INVALID_RESOURCE && !skipShadows) {
        DepthPrePass(cmdBuffer, view, depthStencil, materials, opaqueProxies, frameIndex, width, height);
    }


    // 3. Main Pass Part 1 (Opaque)
    rhi::RenderPassDesc passDesc{};
    passDesc.colorAttachments.resize(1);
    passDesc.colorAttachments[0].texture = renderTarget;
    passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear;
    passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;
    passDesc.colorAttachments[0].clearValue = rhi::ClearValue{ math::v4{ 0.1f, 0.1f, 0.15f, 1.0f } };

    if (depthStencil != rhi::handles::INVALID_RESOURCE) {
        passDesc.depthAttachment.texture = depthStencil;
        // Clear depth if DepthPrePass was skipped (Dawn or no proxies), else load
        if (opaqueProxies.empty() || skipShadows) {
            passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
            passDesc.depthAttachment.clearValue = rhi::ClearValue{ math::v4{ 1.0f, 0.0f, 0.0f, 1.0f } };
        } else {
            passDesc.depthAttachment.loadOp = rhi::LoadAction::Load;
        }
        passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;
    }

    // std::cout << "ForwardRenderer: Beginning Main RenderPass (Opaque)" << std::endl;
    cmdBuffer->BeginRenderPass(passDesc);

    rhi::ViewportDesc viewport;
    viewport.topLeft = {0.0f, 0.0f};
    viewport.size = {static_cast<float>(width), static_cast<float>(height)};
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    cmdBuffer->SetViewport(viewport);

    rhi::Rect scissor;
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<u32>(width), static_cast<u32>(height)};
    cmdBuffer->SetScissor(scissor);

    bool useDepthEqual = (depthStencil != rhi::handles::INVALID_RESOURCE) && !skipShadows;
    OpaquePass(cmdBuffer, view, materials, opaqueProxies, frameIndex, useDepthEqual);

    cmdBuffer->EndRenderPass();

    // 4. SSR Pass (Dawn skips — no SSR support)
    if (!skipShadows && ssrOutput_ == rhi::handles::INVALID_RESOURCE) {
        rhi::TextureDesc desc{
            {width, height, 1},
            1,
            1,
            rhi::DataFormat::RGBA16_Float,
            rhi::TextureType::Texture2D,
            rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess,
            rhi::GPUMemoryUsage::Static,
            "SSROutput"
        };
        ssrOutput_ = device_->CreateTexture(desc);
        
        rhi::DescriptorSetDesc dsDesc;
        dsDesc.layout = compositeDescriptorSetLayout_;
        compositeDescriptorSet_ = device_->CreateDescriptorSet(dsDesc);
        
        rhi::DescriptorImageInfo updateInfo;
        updateInfo.imageView = ssrOutput_;
        updateInfo.sampler = shadowMapSampler_; // Reuse linear sampler
        updateInfo.imageLayout = rhi::ResourceState::ShaderResource;

        rhi::WriteDescriptorSet update;
        update.dstSet = compositeDescriptorSet_;
        update.dstBinding = 0;
        update.descriptorType = rhi::DescriptorType::CombinedImageSampler;
        update.descriptorCount = 1;
        update.imageInfo = &updateInfo;
        
        device_->UpdateDescriptorSets(1, &update);
    }

    {
        rhi::ResourceBarrier barriers[2];
        barriers[0].resource = renderTarget;
        barriers[0].beforeState = rhi::ResourceState::RenderTarget;
        barriers[0].afterState = rhi::ResourceState::ShaderResource;
        barriers[1].resource = depthStencil;
        barriers[1].beforeState = rhi::ResourceState::DepthStencil;
        barriers[1].afterState = rhi::ResourceState::ShaderResource;
        u32 barrierCount = (depthStencil != rhi::handles::INVALID_RESOURCE) ? 2 : 1;
        cmdBuffer->InsertBarrier(barriers, barrierCount);
    }

    // Stage 1: Skip SSR on Dawn (surface texture can't be sampled in compute)
    if (!skipShadows) {
        ssrPass_.Execute(cmdBuffer, renderTarget, depthStencil, ssrOutput_, width, height, frameIndex, view.GetViewMatrix(), view.GetProjectionMatrix());
    }

    {
        rhi::ResourceBarrier barriers[3];
        barriers[0].resource = renderTarget;
        barriers[0].beforeState = rhi::ResourceState::ShaderResource;
        barriers[0].afterState = rhi::ResourceState::RenderTarget;
        barriers[1].resource = depthStencil;
        barriers[1].beforeState = rhi::ResourceState::ShaderResource;
        barriers[1].afterState = rhi::ResourceState::DepthStencil;
        barriers[2].resource = ssrOutput_;
        barriers[2].beforeState = rhi::ResourceState::UnorderedAccess;
        barriers[2].afterState = rhi::ResourceState::ShaderResource;
        u32 barrierCount = (depthStencil != rhi::handles::INVALID_RESOURCE) ? 3 : 2;
        cmdBuffer->InsertBarrier(barriers, barrierCount);
    }

    // 5. Main Pass Part 2 (Composite + Transparent)
    passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Load;
    passDesc.depthAttachment.loadOp = rhi::LoadAction::Load;
    
    // std::cout << "ForwardRenderer: Beginning Main RenderPass (Composite + Transparent)" << std::endl;
    cmdBuffer->BeginRenderPass(passDesc);
    cmdBuffer->SetViewport(viewport);
    cmdBuffer->SetScissor(scissor);

    if (compositePipeline_ != rhi::handles::INVALID_PIPELINE && compositeDescriptorSet_ != rhi::handles::INVALID_RESOURCE) {
        cmdBuffer->BindGraphicsPipeline(compositePipeline_);
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, compositePipelineLayout_, 0, 1, &compositeDescriptorSet_, 0, nullptr);
        cmdBuffer->Draw(3, 1, 0, 0);
    }

    // std::cout << "ForwardRenderer: Calling TransparentPass" << std::endl;
    TransparentPass(cmdBuffer, view, materials, transparentProxies, frameIndex);

#ifndef DISABLE_PARTICLE_SYSTEM
    // 5.5. Particle Pass (after TransparentPass)
    // DISABLED: ParticlePass is now handled by RenderGraph in TestParticleSponza
    // particlePass_.execute(cmdBuffer, frameIndex, view.GetViewMatrix(), view.GetProjectionMatrix());
#endif

    // 6. Geometry Debug Pass
    if (!skipShadows) {
        RenderGeometryDebug(*device_, cmdBuffer, view, renderTarget, depthStencil, rhi::DataFormat::BGRA8_UNorm, rhi::DataFormat::D32_Float, debugSettings_);
    }

    cmdBuffer->EndRenderPass();

    // Mark buffers dirty so FlushStaging only uploads the written portion
    u32 fi = frameIndex % rhi::MAX_FRAMES_IN_FLIGHT;
    device_->SetBufferDirtySize(perObjectBuffers_[fi], perObjectBufferOffset_);
    device_->SetBufferDirtySize(frameBuffers_[fi], sizeof(rhi::GlobalShaderData));
    device_->SetBufferDirtySize(lightBuffers_[fi], sizeof(rhi::ForwardLightBuffer));
}

void ForwardRenderer::DepthPrePass(rhi::RHICommandBuffer* cmdBuffer, 
                                   const RenderView& view, 
                                   rhi::ResourceHandle depthStencil,
                                   const std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>>& materials,
                                   const utl::vector<const RenderProxy*>& proxies,
                                   u32 frameIndex,
                                   u32 width,
                                   u32 height) {
    if (proxies.empty()) return;

    rhi::RenderPassDesc passDesc{};
    passDesc.depthAttachment.texture = depthStencil;
    passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
    passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;
    passDesc.depthAttachment.clearValue = rhi::ClearValue{ math::v4{ 1.0f, 0.0f, 0.0f, 1.0f } };

    cmdBuffer->BeginRenderPass(passDesc);

    rhi::ViewportDesc viewport{ {0.0f, 0.0f}, {static_cast<float>(width), static_cast<float>(height)}, 0.0f, 1.0f };
    cmdBuffer->SetViewport(viewport);

    rhi::Rect scissor{{0, 0}, {static_cast<u32>(width), static_cast<u32>(height)}};
    cmdBuffer->SetScissor(scissor);

    // Bind Global Set (Set 0)
    utl::vector<rhi::DescriptorSetHandle> sets;
    sets.push_back(globalDescriptorSets_[frameIndex]);
    // Note: BindDescriptorSets usually requires pipeline layout. 
    // We get pipeline layout from the first material/pipeline? 
    // Or we assume all pipelines share compatible layouts for Set 0.
    
    for (const auto* proxy : proxies) {
        auto it = materials.find(proxy->materialId);
        if (it == materials.end() || !it->second) continue;
        MaterialInstance* mi = it->second.get();
        Material* mat = mi->GetMaterial();
        
        // Depth Only Pipeline
        rhi::PipelineHandle pipeline = mat->GetPipeline(device_, rhi::handles::INVALID_RESOURCE, 0, PipelineFlags::DepthOnly);
        cmdBuffer->BindGraphicsPipeline(pipeline);

        // Bind Global Set (Set 0)
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 0, 1, &globalDescriptorSets_[frameIndex], 0, nullptr);

        // Update Per-Object Data
        u32 alignedSize = (sizeof(rhi::PerObjectData) + 255) & ~255;
        if (perObjectBufferOffset_ + alignedSize > MAX_PER_OBJECT_SIZE) break;

        auto* perObjectData = reinterpret_cast<rhi::PerObjectData*>(
            static_cast<u8*>(perObjectBuffersMapped_[frameIndex]) + perObjectBufferOffset_);
        
        perObjectData->world = proxy->transform;
        perObjectData->invWorld = rhi::math::Inverse(proxy->transform);
        perObjectData->worldViewProjection = view.GetViewProjectionMatrix() * proxy->transform;

        // Bind Per-Object Set (Set 1) with Dynamic Offset
        u32 dynamicOffset = perObjectBufferOffset_;
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 1, 1, &perObjectDescriptorSets_[frameIndex], 1, &dynamicOffset);

        // Draw
        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->meshId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }

        perObjectBufferOffset_ += alignedSize;
    }

    cmdBuffer->EndRenderPass();
}

void ForwardRenderer::OpaquePass(rhi::RHICommandBuffer* cmdBuffer,
                                 const RenderView& view,
                                 const std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>>& materials,
                                 const utl::vector<const RenderProxy*>& proxies,
                                 u32 frameIndex,
                                 bool useDepthEqual,
                                 rhi::DescriptorSetHandle overrideGlobalSet,
                                 PipelineFlags extraFlags) {
    for (const auto* proxy : proxies) {
        auto it = materials.find(proxy->materialId);
        if (it == materials.end() || !it->second) {
            continue;
        }
        MaterialInstance* mi = it->second.get();
        Material* mat = mi->GetMaterial();

        rhi::PipelineHandle pipeline = rhi::handles::INVALID_PIPELINE;

        // 1. Try to get pipeline from PipelineComponent (ECS)
        pipeline::component pipelineComp{ pipeline::pipeline_id{proxy->entityId} };
        if (pipeline::is_valid(pipelineComp)) {
            pipeline = pipeline::get_pipeline_handle(pipelineComp);
        }

        // 2. Fallback to Material System if no component override
        if (pipeline == rhi::handles::INVALID_PIPELINE) {
            PipelineFlags flags = useDepthEqual ? PipelineFlags::DepthEqual : PipelineFlags::None;
            flags = flags | extraFlags;
            pipeline = mat->GetPipeline(device_, rhi::handles::INVALID_RESOURCE, 0, flags);
        }

        cmdBuffer->BindGraphicsPipeline(pipeline);

        // Bind Sets
        rhi::DescriptorSetHandle globalSet = (overrideGlobalSet != rhi::handles::INVALID_DESCRIPTOR_SET) ? overrideGlobalSet : globalDescriptorSets_[frameIndex];
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 0, 1, &globalSet, 0, nullptr);

        u32 alignedSize = (sizeof(rhi::PerObjectData) + 255) & ~255;
        if (perObjectBufferOffset_ + alignedSize > MAX_PER_OBJECT_SIZE) break;

        auto* perObjectData = reinterpret_cast<rhi::PerObjectData*>(
            static_cast<u8*>(perObjectBuffersMapped_[frameIndex]) + perObjectBufferOffset_);

        perObjectData->world = proxy->transform;
        perObjectData->invWorld = rhi::math::Inverse(proxy->transform);
        perObjectData->worldViewProjection = view.GetViewProjectionMatrix() * proxy->transform;

        u32 dynamicOffset = perObjectBufferOffset_;
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 1, 1, &perObjectDescriptorSets_[frameIndex], 1, &dynamicOffset);

        // Bind Material Set (Set 2? Or whatever Material uses)
        // Assuming MaterialInstance manages a set at index 2
        rhi::DescriptorSetHandle matSet = mi->GetDescriptorSet();
        if (matSet != rhi::handles::INVALID_RESOURCE) {
            cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 2, 1, &matSet, 0, nullptr);
        }

        // Shadow bindings are in Group 0 (global set) — no separate group 3 bind needed

        // Draw
        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->entityId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }

        perObjectBufferOffset_ += alignedSize;
    }
}

void ForwardRenderer::TransparentPass(rhi::RHICommandBuffer* cmdBuffer,
                                      const RenderView& view,
                                      const std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>>& materials,
                                      const utl::vector<const RenderProxy*>& proxies,
                                      u32 frameIndex) {
    for (const auto* proxy : proxies) {
        auto it = materials.find(proxy->materialId);
        if (it == materials.end() || !it->second) continue;
        MaterialInstance* mi = it->second.get();
        Material* mat = mi->GetMaterial();

        rhi::PipelineHandle pipeline = rhi::handles::INVALID_PIPELINE;

        // 1. Try to get pipeline from PipelineComponent (ECS)
        pipeline::component pipelineComp{ pipeline::pipeline_id{proxy->entityId} };
        if (pipeline::is_valid(pipelineComp)) {
            pipeline = pipeline::get_pipeline_handle(pipelineComp);
        }

        // 2. Fallback to Material System
        if (pipeline == rhi::handles::INVALID_PIPELINE) {
            pipeline = mat->GetPipeline(device_, rhi::handles::INVALID_RESOURCE, 0, PipelineFlags::None);
        }

        cmdBuffer->BindGraphicsPipeline(pipeline);

        // Bind Global Set (Set 0)
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 0, 1, &globalDescriptorSets_[frameIndex], 0, nullptr);
        
        u32 alignedSize = (sizeof(rhi::PerObjectData) + 255) & ~255;
        if (perObjectBufferOffset_ + alignedSize > MAX_PER_OBJECT_SIZE) break;

        auto* perObjectData = reinterpret_cast<rhi::PerObjectData*>(
            static_cast<u8*>(perObjectBuffersMapped_[frameIndex]) + perObjectBufferOffset_);
        
        perObjectData->world = proxy->transform;
        perObjectData->invWorld = rhi::math::Inverse(proxy->transform);
        perObjectData->worldViewProjection = view.GetViewProjectionMatrix() * proxy->transform;

        u32 dynamicOffset = perObjectBufferOffset_;
        cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 1, 1, &perObjectDescriptorSets_[frameIndex], 1, &dynamicOffset);

        // Bind Material Set (Set 2)
        rhi::DescriptorSetHandle matSet = mi->GetDescriptorSet();
        if (matSet != rhi::handles::INVALID_RESOURCE) {
            cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 2, 1, &matSet, 0, nullptr);
        }

        // Shadow bindings are in Group 0 (global set) — no separate group 3 bind needed

        // Draw
        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->entityId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }

        perObjectBufferOffset_ += alignedSize;
    }
}

void ForwardRenderer::SetDawnShadowResources(rhi::ResourceHandle depthTex, rhi::SamplerHandle sampler) {
    dawnShadowDepthTex_ = depthTex;
    dawnShadowSampler_ = sampler;

    if (depthTex == rhi::handles::INVALID_RESOURCE || sampler == rhi::handles::INVALID_SAMPLER) return;

    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        if (globalDescriptorSets_[i] == rhi::handles::INVALID_DESCRIPTOR_SET) continue;

        rhi::DescriptorImageInfo imgInfo;
        imgInfo.imageView = depthTex;
        imgInfo.sampler = sampler;

        rhi::WriteDescriptorSet writes[2];
        writes[0].dstSet = globalDescriptorSets_[i];
        writes[0].dstBinding = SHADOW_MAP_BINDING; // 13
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = rhi::DescriptorType::SampledDepthImage;
        writes[0].imageInfo = &imgInfo;

        writes[1].dstSet = globalDescriptorSets_[i];
        writes[1].dstBinding = SHADOW_CUBE_MAP_BINDING; // 14
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = rhi::DescriptorType::Sampler;
        writes[1].imageInfo = &imgInfo;

        device_->UpdateDescriptorSets(2, writes);
    }
}

} // namespace primal::graphics
