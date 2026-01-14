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

#include <algorithm>
#include <iostream>
#include <vector>

namespace primal::graphics {

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
    utl::vector<rhi::DescriptorSetLayoutBinding> globalBindings(2);
    globalBindings[0].binding = FRAME_DATA_BINDING;
    globalBindings[0].descriptorType = rhi::DescriptorType::UniformBuffer;
    globalBindings[0].descriptorCount = 1;
    globalBindings[0].stageFlags = rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel;

    globalBindings[1].binding = LIGHT_DATA_BINDING;
    globalBindings[1].descriptorType = rhi::DescriptorType::UniformBuffer;
    globalBindings[1].descriptorCount = 1;
    globalBindings[1].stageFlags = rhi::ShaderStage::Pixel;

    rhi::DescriptorSetLayoutDesc globalLayoutDesc;
    globalLayoutDesc.bindingCount = 2;
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

    // 6. Allocate and Update Descriptor Sets
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

        rhi::WriteDescriptorSet writes[] = {writeFrame, writeLight};
        device->UpdateDescriptorSets(2, writes);

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

    return true;
}

void ForwardRenderer::Shutdown() {
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
    }
    device_ = nullptr;
}

void ForwardRenderer::SetupLights(const RenderScene& scene, uint32_t frameIndex, rhi::GlobalShaderData* globalData) {
    if (frameIndex >= rhi::MAX_FRAMES_IN_FLIGHT || !lightBuffersMapped_[frameIndex]) return;

    auto* buffer = static_cast<rhi::ForwardLightBuffer*>(lightBuffersMapped_[frameIndex]);
    buffer->directionalLightCount = 0;
    buffer->punctualLightCount = 0;

    const auto& allLights = scene.GetLights();
    for (const auto& light : allLights) {
        if (light.type == LightType::Directional) {
            if (buffer->directionalLightCount < 4) {
                auto& dl = buffer->directionalLights[buffer->directionalLightCount++];
                // dl.lightMVP = ...; // Calculate shadow matrix if needed
                dl.directionAndIntensity = {light.direction.x, light.direction.y, light.direction.z, light.intensity};
                dl.color = {light.color.x, light.color.y, light.color.z, 1.0f};
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
                
                // if (light.type == LightType::Point) pl.type = 1;
                // else if (light.type == LightType::Spot) pl.type = 2;
                // else pl.type = 0;
            }
        }
    }

    if (globalData) {
        globalData->numDirectionalLights = buffer->directionalLightCount;
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
        
        SetupLights(scene, frameIndex, frameData);
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
    OpaquePass(cmdBuffer, view, materials, opaqueProxies, frameIndex, useDepthEqual);
    TransparentPass(cmdBuffer, view, materials, transparentProxies, frameIndex);

    cmdBuffer->EndRenderPass();
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
