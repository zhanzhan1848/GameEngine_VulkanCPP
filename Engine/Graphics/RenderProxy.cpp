#include "RenderProxy.h"
#include "RenderMesh.h"

namespace primal::graphics {

RenderProxy::RenderProxy()
    : transform(rhi::math::CreateTranslationMatrix(math::v3{0.f, 0.f, 0.f})),
      worldAABB(),
      meshId(id::invalid_id),
      materialId(id::invalid_id),
      entityId(id::invalid_id),
      technique(ShaderTechnique::Opaque) {}

RenderProxy::RenderProxy(id::id_type entity, id::id_type mesh, id::id_type material)
    : transform(rhi::math::CreateTranslationMatrix(math::v3{0.f, 0.f, 0.f})),
      worldAABB(),
      meshId(mesh),
      materialId(material),
      entityId(entity),
      technique(ShaderTechnique::Opaque) {
    RecalculateWorldAABB();
}

RenderProxy RenderProxy::Create(id::id_type entityId, id::id_type meshId, id::id_type materialId) {
    return RenderProxy(entityId, meshId, materialId);
}

void RenderProxy::UpdateTransform(const math::m4x4& newTransform) {
    transform = newTransform;
    RecalculateWorldAABB();
}

void RenderProxy::SetMaterial(id::id_type newMaterialId) {
    materialId = newMaterialId;
}

void RenderProxy::RecalculateWorldAABB() {
    RenderMesh* mesh = RenderMesh::GetByEntityId(meshId);
    if (mesh) {
        const rhi::AABB& localAABB = mesh->GetLocalAABB();
        worldAABB = localAABB.Transform(transform);
    } else {
        // 如果没有 Mesh，使用一个非常小的默认包围盒或标记为无效
        // 这里我们创建一个以原点为中心的单位小盒子
        rhi::AABB defaultAABB(math::v3{-0.1f, -0.1f, -0.1f}, math::v3{0.1f, 0.1f, 0.1f});
        worldAABB = defaultAABB.Transform(transform);
    }
}

} // namespace primal::graphics
