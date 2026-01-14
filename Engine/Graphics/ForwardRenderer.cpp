#include "ForwardRenderer.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIMath.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace primal::graphics {

// 辅助结构体：对象统一缓冲区数据
// 注意：这必须与 Shader 中的 Uniform Block 定义匹配
struct ObjectUniform {
    math::m4x4 world;
    math::m4x4 view;
    math::m4x4 proj;
    math::m4x4 viewProj;
};

ForwardRenderer::ForwardRenderer() = default;
ForwardRenderer::~ForwardRenderer() = default;

bool ForwardRenderer::Initialize(rhi::RHIDeviceBase* device) {
    if (!device) return false;
    device_ = device;
    return true;
}

void ForwardRenderer::Shutdown() {
    device_ = nullptr;
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
    (void)scene; // Unused for now

    // 0. Filter and Sort Proxies
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

    // Sort Opaque (Front-to-Back)
    rhi::math::m4x4 viewInv = rhi::math::Inverse(view.GetViewMatrix());
    rhi::math::v3 cameraPos = {viewInv.columns[3][0], viewInv.columns[3][1], viewInv.columns[3][2]};

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

    // 1. Z-Prepass (Depth Only)
    // Only perform Z-Prepass if we have a valid depth buffer
    if (depthStencil != rhi::handles::INVALID_RESOURCE) {
        DepthPrePass(cmdBuffer, view, depthStencil, materials, opaqueProxies, frameIndex, width, height);
    }

    // 2. Main Pass (Opaque + Transparent)
    rhi::RenderPassDesc passDesc{};
    
    // Color Attachment
    passDesc.colorAttachments.resize(1);
    passDesc.colorAttachments[0].texture = renderTarget;
    passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear;
    passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;
    passDesc.colorAttachments[0].clearValue = rhi::ClearValue(0.1f, 0.1f, 0.1f, 1.0f); // 默认背景色

    // Depth Attachment
    if (depthStencil != rhi::handles::INVALID_RESOURCE) {
        passDesc.depthAttachment.texture = depthStencil;
        passDesc.depthAttachment.loadOp = rhi::LoadAction::Load; // Load depth from PrePass
        passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;
        // clearValue is ignored for Load
    }

    // 3. 开始 Render Pass
    cmdBuffer->BeginRenderPass(passDesc);

    // 4. 设置全局状态 (Viewport, Scissor)
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

    // 5. 执行 Pass
    OpaquePass(cmdBuffer, view, materials, opaqueProxies, frameIndex);
    TransparentPass(cmdBuffer, view, materials, transparentProxies, frameIndex);

    // 6. 结束 Render Pass
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
    // 0. Check if anything to draw
    if (proxies.empty()) {
        // We still need to clear depth buffer for Main Pass
        // TODO: Optimize by coordinating with Main Pass LoadOp
        // For now, just clear
        rhi::RenderPassDesc passDesc{};
        passDesc.depthAttachment.texture = depthStencil;
        passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
        passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;
        passDesc.depthAttachment.clearValue = rhi::ClearValue(1.0f, 0);
        
        cmdBuffer->BeginRenderPass(passDesc);
        cmdBuffer->EndRenderPass();
        return;
    }

    // 1. Setup Render Pass
    rhi::RenderPassDesc passDesc{};
    passDesc.depthAttachment.texture = depthStencil;
    passDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
    passDesc.depthAttachment.storeOp = rhi::StoreAction::Store;
    passDesc.depthAttachment.clearValue = rhi::ClearValue(1.0f, 0);
    // No color attachments

    cmdBuffer->BeginRenderPass(passDesc);

    // 2. Setup Viewport/Scissor
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

    // 3. Draw Depth Only
    for (const auto* proxy : proxies) {
        auto it = materials.find(proxy->materialId);
        MaterialInstance* materialInstance = it->second;
        Material* material = materialInstance->GetMaterial();

        materialInstance->SetCurrentFrame(frameIndex);

        ObjectUniform objectData;
        objectData.world = proxy->transform;
        objectData.view = view.GetViewMatrix();
        objectData.proj = view.GetProjectionMatrix();
        objectData.viewProj = view.GetViewProjectionMatrix();
        
        materialInstance->SetUniformData(0, &objectData, sizeof(ObjectUniform));
        materialInstance->Update(device_);

        // Request Depth Only Pipeline
        rhi::PipelineHandle pipeline = material->GetPipeline(device_, rhi::handles::INVALID_RESOURCE, 0, PipelineFlags::DepthOnly);
        if (pipeline == rhi::handles::INVALID_PIPELINE) continue;

        cmdBuffer->BindGraphicsPipeline(pipeline);

        rhi::DescriptorSetHandle set = materialInstance->GetDescriptorSet();
        if (set != rhi::handles::INVALID_RESOURCE) {
            cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, material->GetPipelineLayout(), 0, 1, &set, 0, nullptr);
        }

        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->meshId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }
    }

    cmdBuffer->EndRenderPass();
}

void ForwardRenderer::OpaquePass(rhi::RHICommandBuffer* cmdBuffer, 
                                 const RenderView& view, 
                                 const std::unordered_map<id::id_type, MaterialInstance*>& materials,
                                 const utl::vector<const RenderProxy*>& proxies,
                                 uint32_t frameIndex) {
    
    if (proxies.empty()) return;

    // 绘制
    for (const auto* proxy : proxies) {
        auto it = materials.find(proxy->materialId);
        MaterialInstance* materialInstance = it->second;
        Material* material = materialInstance->GetMaterial();

        // 设置当前帧索引 (for multi-buffering)
        materialInstance->SetCurrentFrame(frameIndex);

        // 更新 Per-Object Uniforms
        // TODO: 使用 Dynamic Uniform Buffer 或 Push Constants 以提高性能
        ObjectUniform objectData;
        objectData.world = proxy->transform;
        objectData.view = view.GetViewMatrix();
        objectData.proj = view.GetProjectionMatrix();
        objectData.viewProj = view.GetViewProjectionMatrix();
        
        // 假设 Uniform Block 的第一个槽位是 Object Data
        // 注意：如果 MaterialInstance 是共享的，这会导致并发问题或覆盖
        // 正确的做法是 MaterialInstance 只包含 Material Data，Object Data 另行绑定
        materialInstance->SetUniformData(0, &objectData, sizeof(ObjectUniform));
        materialInstance->Update(device_);

        // 获取 Pipeline
        rhi::PipelineHandle pipeline = material->GetPipeline(device_, rhi::handles::INVALID_RESOURCE, 0);
        if (pipeline == rhi::handles::INVALID_PIPELINE) continue;

        cmdBuffer->BindGraphicsPipeline(pipeline);

        rhi::DescriptorSetHandle set = materialInstance->GetDescriptorSet();
        if (set != rhi::handles::INVALID_RESOURCE) {
            cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, material->GetPipelineLayout(), 0, 1, &set, 0, nullptr);
        }

        // 绘制 Mesh
        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->meshId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }
    }
}

void ForwardRenderer::TransparentPass(rhi::RHICommandBuffer* cmdBuffer, 
                                      const RenderView& view, 
                                      const std::unordered_map<id::id_type, MaterialInstance*>& materials,
                                      const utl::vector<const RenderProxy*>& proxies,
                                      uint32_t frameIndex) {
    if (proxies.empty()) return;

    // 绘制
    for (const auto* proxy : proxies) {
        auto it = materials.find(proxy->materialId);
        MaterialInstance* materialInstance = it->second;
        Material* material = materialInstance->GetMaterial();

        materialInstance->SetCurrentFrame(frameIndex);

        ObjectUniform objectData;
        objectData.world = proxy->transform;
        objectData.view = view.GetViewMatrix();
        objectData.proj = view.GetProjectionMatrix();
        objectData.viewProj = view.GetViewProjectionMatrix();
        
        materialInstance->SetUniformData(0, &objectData, sizeof(ObjectUniform));
        materialInstance->Update(device_);

        rhi::PipelineHandle pipeline = material->GetPipeline(device_, rhi::handles::INVALID_RESOURCE, 0);
        if (pipeline == rhi::handles::INVALID_PIPELINE) continue;

        cmdBuffer->BindGraphicsPipeline(pipeline);

        rhi::DescriptorSetHandle set = materialInstance->GetDescriptorSet();
        if (set != rhi::handles::INVALID_RESOURCE) {
            cmdBuffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, material->GetPipelineLayout(), 0, 1, &set, 0, nullptr);
        }

        RenderMesh* mesh = RenderMesh::GetByEntityId(proxy->meshId);
        if (mesh && mesh->IsValid()) {
            mesh->Draw(cmdBuffer);
        }
    }
}

} // namespace primal::graphics
