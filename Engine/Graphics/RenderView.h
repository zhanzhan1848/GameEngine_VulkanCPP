#pragma once
#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RenderScene.h"
#include <vector>

namespace primal::graphics {

/**
 * @brief 渲染视图
 * @details 代表一个观察视角，包含视锥体、变换矩阵和剔除结果
 */
class RenderView {
public:
    RenderView();

    /**
     * @brief 设置视图矩阵
     * @param view 视图矩阵
     */
    void SetViewMatrix(const math::m4x4& view);

    /**
     * @brief 设置投影矩阵
     * @param proj 投影矩阵
     */
    void SetProjectionMatrix(const math::m4x4& proj);

    /**
     * @brief 更新视锥体
     * @details 根据当前的 View 和 Projection 矩阵重新计算视锥体
     */
    void UpdateFrustum();

    /**
     * @brief 对场景执行剔除
     * @param scene 渲染场景
     */
    void Cull(const RenderScene& scene);

    // Getters
    const math::m4x4& GetViewMatrix() const { return viewMatrix_; }
    const math::m4x4& GetProjectionMatrix() const { return projectionMatrix_; }
    const math::m4x4& GetViewProjectionMatrix() const { return viewProjectionMatrix_; }
    const rhi::Frustum& GetFrustum() const { return frustum_; }
    const utl::vector<const RenderProxy*>& GetVisibleProxies() const { return visibleProxies_; }

private:
    math::m4x4 viewMatrix_;
    math::m4x4 projectionMatrix_;
    math::m4x4 viewProjectionMatrix_;
    rhi::Frustum frustum_;

    utl::vector<const RenderProxy*> visibleProxies_;
};

} // namespace primal::graphics
