#include "ForwardRenderer.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIDescriptorSetLayout.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"
#include "Graphics/RHI/Utils/ShadowUtils.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace primal::graphics {

constexpr uint32_t MAX_CSM_CASCADES = 4;
constexpr uint32_t MAX_SPOT_SHADOWS = 4;
constexpr uint32_t SHADOW_MAP_ARRAY_SIZE = MAX_CSM_CASCADES + MAX_SPOT_SHADOWS;
constexpr uint32_t MAX_POINT_SHADOWS = 2;

ForwardRenderer::ForwardRenderer() = default;
ForwardRenderer::~ForwardRenderer() = default;

bool ForwardRenderer::Initialize(rhi::RHIDeviceBase* device) {
    if (!device) return false;
    device_ = device;

    // 1. Create Light Buffers
    rhi::BufferDesc lightBufferDesc;
    lightBufferDesc.size = sizeof(rhi::ForwardLightBuffer);
    lightBufferDesc.type = rhi::BufferType::Constant;
    lightBufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    lightBufferDesc.bindFlags = static_cast<uint32_t>(rhi::ResourceUsage::ConstantBuffer);

    // 2. Create Frame Buffers (GlobalShaderData)
    rhi::BufferDesc frameBufferDesc;
    frameBufferDesc.size = sizeof(rhi::GlobalShaderData);
    frameBufferDesc.type = rhi::BufferType::Constant;
    frameBufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    frameBufferDesc.bindFlags = static_cast<uint32_t>(rhi::ResourceUsage::ConstantBuffer);

    // 3. Create Per-Object Buffers
    rhi::BufferDesc perObjectBufferDesc;
    perObjectBufferDesc.size = MAX_PER_OBJECT_SIZE;
    perObjectBufferDesc.type = rhi::BufferType::Constant; // Using Dynamic Offset
    perObjectBufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    perObjectBufferDesc.bindFlags = static_cast<uint32_t>(rhi::ResourceUsage::ConstantBuffer);

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
    // Binding 0: Frame Data (GlobalShaderData)
    // Binding 2: Light Data (ForwardLightBuffer)
    // Binding 3: Shadow Map (Texture2DArray)
    // Binding 4: Shadow Cube Map (TextureCubeArray)
    utl::vector<rhi::DescriptorSetLayoutBinding> globalBindings(4);
    globalBindings[0].binding = FRAME_DATA_BINDING;
    globalBindings[0].descriptorType = rhi::DescriptorType::UniformBuffer;
    globalBindings[0].descriptorCount = 1;
    globalBindings[0].stageFlags = rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel;

    globalBindings[1].binding = LIGHT_DATA_BINDING;
    globalBindings[1].descriptorType = rhi::DescriptorType::UniformBuffer;
    globalBindings[1].descriptorCount = 1;
    globalBindings[1].stageFlags = rhi::ShaderStage::Pixel;

    globalBindings[2].binding = SHADOW_MAP_BINDING;
    globalBindings[2].descriptorType = rhi::DescriptorType::CombinedImageSampler;
    globalBindings[2].descriptorCount = 1;
    globalBindings[2].stageFlags = rhi::ShaderStage::Pixel;

    globalBindings[3].binding = SHADOW_CUBE_MAP_BINDING;
    globalBindings[3].descriptorType = rhi::DescriptorType::CombinedImageSampler;
    globalBindings[3].descriptorCount = 1;
    globalBindings[3].stageFlags = rhi::ShaderStage::Pixel;

    rhi::DescriptorSetLayoutDesc globalLayoutDesc;
    globalLayoutDesc.bindingCount = 4;
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
    // Shared Depth Buffer (Transient for Shadow Passes)
    rhi::TextureDesc shadowDepthDesc{};
    shadowDepthDesc.size = {2048, 2048, 1};
    shadowDepthDesc.mipLevels = 1;
    shadowDepthDesc.arraySize = 1;
    shadowDepthDesc.format = rhi::DataFormat::D32_Float;
    shadowDepthDesc.type = rhi::TextureType::Texture2D;
    shadowDepthDesc.usage = rhi::TextureUsage::DepthStencil; // Only for depth test, not sampled
    shadowDepthDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    shadowDepthDesc.name = "ShadowDepthBuffer";
    
    shadowDepthBuffer_ = device->CreateTexture(shadowDepthDesc);
    if (shadowDepthBuffer_ == rhi::handles::INVALID_RESOURCE) return false;

    // Shadow Map Array (VSM Color Target: Depth, Depth^2)
    rhi::TextureDesc shadowMapDesc{};
    shadowMapDesc.size = {2048, 2048, 1};
    shadowMapDesc.arraySize = SHADOW_MAP_ARRAY_SIZE; // Cascades + Spot Shadows
    shadowMapDesc.mipLevels = 1;
    shadowMapDesc.format = rhi::DataFormat::RG32_Float;
    shadowMapDesc.type = rhi::TextureType::Texture2DArray;
    shadowMapDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
    shadowMapDesc.memoryUsage = rhi::GPUMemoryUsage::Static; // GPU Only
    shadowMapDesc.name = "ShadowMapArray";
    
    shadowMapArray_ = device->CreateTexture(shadowMapDesc);
    if (shadowMapArray_ == rhi::handles::INVALID_RESOURCE) return false;

    // Shadow Map Temp Array (For Blur Pass)
    shadowMapDesc.name = "ShadowMapTempArray";
    shadowMapTempArray_ = device->CreateTexture(shadowMapDesc);
    if (shadowMapTempArray_ == rhi::handles::INVALID_RESOURCE) return false;

    // Create Shadow Cube Map Array (VSM Color Target)
    rhi::TextureDesc shadowCubeMapDesc{};
    shadowCubeMapDesc.size = {1024, 1024, 1}; // Point Shadows might be smaller
    shadowCubeMapDesc.arraySize = MAX_POINT_SHADOWS * 6; // Total faces
    shadowCubeMapDesc.mipLevels = 1;
    shadowCubeMapDesc.format = rhi::DataFormat::RG32_Float;
    shadowCubeMapDesc.type = rhi::TextureType::TextureCubeArray;
    shadowCubeMapDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    shadowCubeMapDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
    shadowCubeMapDesc.name = "ShadowCubeMapArray";

    shadowCubeMapArray_ = device->CreateTexture(shadowCubeMapDesc);
    if (shadowCubeMapArray_ == rhi::handles::INVALID_RESOURCE) return false;

    rhi::SamplerDesc shadowSamplerDesc{};
    shadowSamplerDesc.minFilter = rhi::FilterMode::Linear;
    shadowSamplerDesc.magFilter = rhi::FilterMode::Linear;
    shadowSamplerDesc.addressU = rhi::TextureAddressMode::Clamp;
    shadowSamplerDesc.addressV = rhi::TextureAddressMode::Clamp;
    shadowSamplerDesc.addressW = rhi::TextureAddressMode::Clamp;
    // shadowSamplerDesc.comparisonFunc = rhi::ComparisonFunc::Less; // Removed for VSM (RG32 Sampling)
    shadowSamplerDesc.borderColor = {1.0f, 1.0f, 1.0f, 1.0f};
    
    shadowMapSampler_ = device->CreateSampler(shadowSamplerDesc);
    // Note: Sampler might not be strictly needed here if we bind it in descriptor set, 
    // but good to have.
    shadowCubeMapSampler_ = device->CreateSampler(shadowSamplerDesc); // Reuse same sampler desc

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

        rhi::WriteDescriptorSet writes[] = {writeFrame, writeLight, writeShadow, writeShadowCube};
        device->UpdateDescriptorSets(4, writes);

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
    if (!blurPass_.Initialize(device_)) {
        std::cerr << "ForwardRenderer: Failed to initialize BlurPass" << std::endl;
        return false;
    }

    return true;
}

void ForwardRenderer::Shutdown() {
    blurPass_.Shutdown();

    if (device_) {
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

void ForwardRenderer::ShadowPass(rhi::RHICommandBuffer* cmdBuffer, 
                                 const RenderView& view, 
                                 rhi::ResourceHandle shadowMap,
                                 const std::unordered_map<id::id_type, MaterialInstance*>& materials,
                                 const utl::vector<const RenderProxy*>& proxies,
                                 uint32_t frameIndex,
                                 uint32_t cascadeIndex) {
    // Ensure we run the pass to Clear the texture even if no proxies are visible
    // if (proxies.empty()) return;

    rhi::RenderPassDesc passDesc{};
    
    // VSM Shadow Pass: Output (Depth, Depth^2) to Color Attachment
    // Use shared depth buffer for Z-Test
    passDesc.colorAttachments.resize(1);
    passDesc.colorAttachments[0].texture = shadowMap;
    passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear; 
    passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;
    passDesc.colorAttachments[0].clearValue = rhi::ClearValue(1.0f, 1.0f, 1.0f, 1.0f);
    passDesc.colorAttachments[0].arrayLayer = static_cast<uint16_t>(cascadeIndex);

    passDesc.depthAttachment.texture = shadowDepthBuffer_;
    passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear; 
    passDesc.depthAttachment.storeOp = rhi::StoreAction::DontCare;
    passDesc.depthAttachment.clearValue = rhi::ClearValue(1.0f, 0);
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
        MaterialInstance* mi = it->second;
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
                                  uint32_t frameIndex, 
                                  rhi::GlobalShaderData* globalData,
                                  const utl::vector<RenderView>& shadowViews,
                                  const utl::vector<float>& splits,
                                  const std::unordered_map<uint32_t, int>& lightShadowIndices,
                                  const std::unordered_map<uint32_t, rhi::math::m4x4>& lightViewProjs) {
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
                auto it = lightShadowIndices.find(static_cast<uint32_t>(i));
                pl.shadowIndex = (it != lightShadowIndices.end()) ? it->second : -1;

                // Check view projection (for Spot Lights mainly)
                auto itVP = lightViewProjs.find(static_cast<uint32_t>(i));
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
                             const std::unordered_map<id::id_type, MaterialInstance*>& materials,
                             uint32_t frameIndex,
                             uint32_t width,
                             uint32_t height) {
    if (!device_ || !cmdBuffer) return;
    (void)scene; 

    // Reset Per-Object Buffer Offset
    perObjectBufferOffset_ = 0;

    // Shadow Pass (Before Main Pass)
    utl::vector<RenderView> csmViews;
    utl::vector<float> cascadeSplits;
    std::unordered_map<uint32_t, int> lightShadowIndices;
    std::unordered_map<uint32_t, rhi::math::m4x4> lightViewProjs;

    // Process Lights for Shadows
    const auto& allLights = scene.GetLights();
    rhi::math::v3 lightDir = {0, -1, 0};
    bool hasDirectionalLight = false;
    uint32_t spotShadowCount = 0;
    uint32_t pointShadowCount = 0;

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

                for (uint32_t j = 0; j < csmViews.size(); ++j) {
                    auto& shadowView = csmViews[j];
                    shadowView.Cull(scene);
                    ShadowPass(cmdBuffer, shadowView, shadowMapArray_, materials, shadowView.GetVisibleProxies(), frameIndex, j);
                }
            }
        } 
        else if (light.type == LightType::Spot) {
            if (spotShadowCount < MAX_SPOT_SHADOWS && shadowMapArray_ != rhi::handles::INVALID_RESOURCE) {
                 uint32_t shadowIndex = MAX_CSM_CASCADES + spotShadowCount;
                 RenderView shadowView;
                 utils::CreateSpotShadowView(light.position, light.direction, light.outerCone, light.range, 2048, shadowView);
                 shadowView.Cull(scene);
                 
                 ShadowPass(cmdBuffer, shadowView, shadowMapArray_, materials, shadowView.GetVisibleProxies(), frameIndex, shadowIndex);
                 
                 lightShadowIndices[static_cast<uint32_t>(i)] = shadowIndex;
                 lightViewProjs[static_cast<uint32_t>(i)] = shadowView.GetViewProjectionMatrix();
                 spotShadowCount++;
            }
        }
        else if (light.type == LightType::Point) {
            if (pointShadowCount < MAX_POINT_SHADOWS && shadowCubeMapArray_ != rhi::handles::INVALID_RESOURCE) {
                uint32_t baseSlice = pointShadowCount * 6;
                utl::vector<RenderView> pointViews;
                utils::CreatePointShadowViews(light.position, light.range, 1024, pointViews);
                
                for(int face = 0; face < 6; ++face) {
                    auto& shadowView = pointViews[face];
                    shadowView.Cull(scene);
                    ShadowPass(cmdBuffer, shadowView, shadowCubeMapArray_, materials, shadowView.GetVisibleProxies(), frameIndex, baseSlice + face);
                }
                
                lightShadowIndices[static_cast<uint32_t>(i)] = pointShadowCount;
                pointShadowCount++;
            }
        }
    }

    // Execute Blur Pass for VSM (ShadowMapArray -> Temp -> ShadowMapArray)
    if (shadowMapArray_ != rhi::handles::INVALID_RESOURCE && shadowMapTempArray_ != rhi::handles::INVALID_RESOURCE) {
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

    // 2. Z-Prepass (Depth Only)
    if (depthStencil != rhi::handles::INVALID_RESOURCE) {
        DepthPrePass(cmdBuffer, view, depthStencil, materials, opaqueProxies, frameIndex, width, height);
    }

    // 3. Main Pass (Opaque + Transparent)
    rhi::RenderPassDesc passDesc{};
    passDesc.colorAttachments.resize(1);
    passDesc.colorAttachments[0].texture = renderTarget;
    passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear;
    passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;
    passDesc.colorAttachments[0].clearValue = rhi::ClearValue(0.1f, 0.1f, 0.1f, 1.0f); 

    if (depthStencil != rhi::handles::INVALID_RESOURCE) {
        passDesc.depthAttachment.texture = depthStencil;
        passDesc.depthAttachment.loadOp = rhi::LoadAction::Load; 
        passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;
    }

    std::cout << "ForwardRenderer: Beginning Main RenderPass" << std::endl;
    cmdBuffer->BeginRenderPass(passDesc);

    rhi::ViewportDesc viewport{};
    viewport.topLeft = {0.0f, 0.0f};
    viewport.size = {static_cast<float>(width), static_cast<float>(height)};
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    cmdBuffer->SetViewport(viewport);

    rhi::Rect scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    cmdBuffer->SetScissor(scissor);

    bool useDepthEqual = (depthStencil != rhi::handles::INVALID_RESOURCE);
    // std::cout << "ForwardRenderer: Calling OpaquePass" << std::endl;
    OpaquePass(cmdBuffer, view, materials, opaqueProxies, frameIndex, useDepthEqual);
    // std::cout << "ForwardRenderer: Calling TransparentPass" << std::endl;
    TransparentPass(cmdBuffer, view, materials, transparentProxies, frameIndex);

    cmdBuffer->EndRenderPass();
    // std::cout << "ForwardRenderer: Main RenderPass Ended" << std::endl;
}

void ForwardRenderer::DepthPrePass(rhi::RHICommandBuffer* cmdBuffer, 
                                   const RenderView& view, 
                                   rhi::ResourceHandle depthStencil,
                                   const std::unordered_map<id::id_type, MaterialInstance*>& materials,
                                   const utl::vector<const RenderProxy*>& proxies,
                                   uint32_t frameIndex,
                                   uint32_t width,
                                   uint32_t height) {
    if (proxies.empty()) return;

    rhi::RenderPassDesc passDesc{};
    passDesc.depthAttachment.texture = depthStencil;
    passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
    passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;
    passDesc.depthAttachment.clearValue = rhi::ClearValue(1.0f, 0);

    cmdBuffer->BeginRenderPass(passDesc);

    rhi::ViewportDesc viewport{};
    viewport.topLeft = {0.0f, 0.0f};
    viewport.size = {static_cast<float>(width), static_cast<float>(height)};
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    cmdBuffer->SetViewport(viewport);

    rhi::Rect scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
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
        MaterialInstance* mi = it->second;
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
                                 const std::unordered_map<id::id_type, MaterialInstance*>& materials,
                                 const utl::vector<const RenderProxy*>& proxies,
                                 uint32_t frameIndex,
                                 bool useDepthEqual) {
    for (const auto* proxy : proxies) {
        auto it = materials.find(proxy->materialId);
        if (it == materials.end() || !it->second) continue;
        MaterialInstance* mi = it->second;
        Material* mat = mi->GetMaterial();

        PipelineFlags flags = useDepthEqual ? PipelineFlags::DepthEqual : PipelineFlags::None;
        rhi::PipelineHandle pipeline = mat->GetPipeline(device_, rhi::handles::INVALID_RESOURCE, 0, flags);
        cmdBuffer->BindGraphicsPipeline(pipeline);

        // Bind Sets
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

        // Bind Material Set (Set 2? Or whatever Material uses)
        // Assuming MaterialInstance manages a set at index 2
        rhi::DescriptorSetHandle matSet = mi->GetDescriptorSet();
        if (matSet != rhi::handles::INVALID_RESOURCE) {
            cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, mat->GetPipelineLayout(), 2, 1, &matSet, 0, nullptr);
        }

        // Draw
        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->meshId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }

        perObjectBufferOffset_ += alignedSize;
    }
}

void ForwardRenderer::TransparentPass(rhi::RHICommandBuffer* cmdBuffer, 
                                      const RenderView& view, 
                                      const std::unordered_map<id::id_type, MaterialInstance*>& materials,
                                      const utl::vector<const RenderProxy*>& proxies,
                                      uint32_t frameIndex) {
    for (const auto* proxy : proxies) {
        auto it = materials.find(proxy->materialId);
        if (it == materials.end() || !it->second) continue;
        MaterialInstance* mi = it->second;
        Material* mat = mi->GetMaterial();

        rhi::PipelineHandle pipeline = mat->GetPipeline(device_, rhi::handles::INVALID_RESOURCE, 0, PipelineFlags::None);
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

        // Draw
        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->meshId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }

        perObjectBufferOffset_ += alignedSize;
    }
}

} // namespace primal::graphics
