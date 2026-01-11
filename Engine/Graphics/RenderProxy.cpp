#include "RenderProxy.h"

namespace primal::graphics {

RenderProxy::RenderProxy()
    : transform(rhi::math::CreateTranslationMatrix(math::v3{0.f, 0.f, 0.f})),
      meshId(id::invalid_id),
      materialId(id::invalid_id),
      entityId(id::invalid_id) {}

RenderProxy::RenderProxy(id::id_type entity, id::id_type mesh, id::id_type material)
    : transform(rhi::math::CreateTranslationMatrix(math::v3{0.f, 0.f, 0.f})),
      meshId(mesh),
      materialId(material),
      entityId(entity) {}

RenderProxy RenderProxy::Create(id::id_type entityId, id::id_type meshId, id::id_type materialId) {
    return RenderProxy(entityId, meshId, materialId);
}

void RenderProxy::UpdateTransform(const math::m4x4& newTransform) {
    transform = newTransform;
}

void RenderProxy::SetMaterial(id::id_type newMaterialId) {
    materialId = newMaterialId;
}

} // namespace primal::graphics
