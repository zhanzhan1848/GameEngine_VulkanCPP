#include "RHISceneSynchronizer.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/RHI/Core/RHIEntityManager.h"
#include "Graphics/RHI/Components/RenderLayerComponent.h"
#include "Graphics/RHI/Components/MaterialComponent.h"
#include "Graphics/RHI/Components/GPUBufferComponent.h"
#include "Graphics/RHI/Components/RHITransformComponent.h"
#include "Graphics/RHI/Systems/RenderSystem.h"
#include <unordered_set>

namespace primal::graphics::rhi {

    RHISceneSynchronizer::RHISceneSynchronizer() = default;
    RHISceneSynchronizer::~RHISceneSynchronizer() = default;

    void RHISceneSynchronizer::Initialize(RHIEntityManager* entityManager, RHIDeviceBase* device, RenderSystem* renderSystem) {
        entityManager_ = entityManager;
        device_ = device;
        renderSystem_ = renderSystem;
    }

    void RHISceneSynchronizer::Synchronize(const RenderScene& renderScene) {
        if (!entityManager_ || !device_ || !renderSystem_) return;

        const auto& proxies = renderScene.GetProxies();
        std::unordered_set<id::id_type> currentFrameEntities;

        for (const auto& proxy : proxies) {
            id::id_type gameEntityId = proxy.entityId;
            currentFrameEntities.insert(gameEntityId);

            RHIEntityID rhiEntityId = 0;
            bool isNew = false;

            // 1. 查找或创建 RHI 实体
            auto it = entityMap_.find(gameEntityId);
            if (it != entityMap_.end()) {
                rhiEntityId = it->second;
            } else {
                rhiEntityId = entityManager_->CreateEntity();
                entityMap_[gameEntityId] = rhiEntityId;
                isNew = true;
            }

            // 2. 同步组件

            // RenderLayerComponent
            if (isNew) {
                auto& layer = entityManager_->AddComponent<RenderLayerComponent>(rhiEntityId);
                layer.layerMask = 1; // Default Layer
                layer.priority = 0;
            }

            // MaterialComponent
            auto* materialComp = entityManager_->GetComponent<MaterialComponent>(rhiEntityId);
            if (!materialComp) {
                materialComp = &entityManager_->AddComponent<MaterialComponent>(rhiEntityId);
            }
            
            // 检查材质是否变化
        if (proxy.materialId != id::invalid_id) {
            std::shared_ptr<MaterialInstance> matInst = renderSystem_->GetMaterialInstance(proxy.materialId);
            if (matInst) {
                if (!materialComp->materialInstance || materialComp->materialInstance != matInst) {
                    materialComp->materialInstance = matInst;
                }
            }
        }

            // GPUBufferComponent
            auto* bufferComp = entityManager_->GetComponent<GPUBufferComponent>(rhiEntityId);
            if (!bufferComp) {
                bufferComp = &entityManager_->AddComponent<GPUBufferComponent>(rhiEntityId);
            }

            if (proxy.meshId != id::invalid_id) {
                RenderMesh* mesh = RenderMesh::GetByEntityId(proxy.meshId);
                if (mesh) {
                    bufferComp->vertexBuffer = mesh->GetVertexBuffer();
                    bufferComp->indexBuffer = mesh->GetIndexBuffer();
                    bufferComp->vertexCount = mesh->GetVertexCount();
                    bufferComp->indexCount = mesh->GetIndexCount();
                    // bufferComp->indexType = mesh->GetIndexType(); // Need to expose IndexType in RenderMesh
                    bufferComp->offset = 0;
                    bufferComp->indexOffset = 0;
                }
            }

            // RHITransformComponent
            auto* transformComp = entityManager_->GetComponent<RHITransformComponent>(rhiEntityId);
            if (!transformComp) {
                transformComp = &entityManager_->AddComponent<RHITransformComponent>(rhiEntityId);
            }
            transformComp->worldMatrix = proxy.transform;
        }

        // 3. 清理已移除的实体
        for (auto it = entityMap_.begin(); it != entityMap_.end(); ) {
            if (currentFrameEntities.find(it->first) == currentFrameEntities.end()) {
                entityManager_->DestroyEntity(it->second);
                it = entityMap_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void RHISceneSynchronizer::Clear() {
        if (entityManager_) {
            for (auto& pair : entityMap_) {
                entityManager_->DestroyEntity(pair.second);
            }
        }
        entityMap_.clear();
    }

}
