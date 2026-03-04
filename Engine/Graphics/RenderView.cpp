#include "RenderView.h"

namespace primal::graphics {

RenderView::RenderView() 
    : viewMatrix_(rhi::math::MatrixIdentity()),
      projectionMatrix_(rhi::math::MatrixIdentity()),
      viewProjectionMatrix_(rhi::math::MatrixIdentity()) {
    UpdateFrustum();
}

void RenderView::SetViewMatrix(const math::m4x4& view) {
    viewMatrix_ = view;
    UpdateFrustum();
}

void RenderView::SetProjectionMatrix(const math::m4x4& proj) {
    projectionMatrix_ = proj;
    UpdateFrustum();
}

void RenderView::UpdateFrustum() {
    viewProjectionMatrix_ = projectionMatrix_ * viewMatrix_;
    frustum_.FromMatrix(viewProjectionMatrix_);
}

void RenderView::Cull(const RenderScene& scene) {
    visibleProxies_.clear();
    scene.Cull(frustum_, visibleProxies_);
}

} // namespace primal::graphics
