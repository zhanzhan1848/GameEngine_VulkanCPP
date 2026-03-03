#pragma once
#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RenderScene.h"
#include <vector>

namespace primal::graphics {

/**
 * @brief 渲染视图类型
 */
enum class ViewType {
    Main,           ///< 主相机视图
    ShadowMap,      ///< 阴影贴图视图 (CSM, Spot)
    Reflection,     ///< 平面反射视图
    CubeMap,        ///< 环境探针/点光源阴影视图
    Unknown
};

/**
 * @brief 阴影视图特定信息
 */
struct ShadowViewInfo {
    u32 cascadeIndex{0};       ///< CSM 级联索引
    float splitDistance{0.0f};      ///< CSM 分割距离
    float bias{0.005f};             ///< 深度偏移
    float slopeBias{0.002f};        ///< 斜率深度偏移
};

/**
 * @brief 渲染视图
 * @details 代表一个观察视角，包含视锥体、变换矩阵和剔除结果
 */
class RenderView {
public:
    RenderView();

    /**
     * @brief 设置视图类型
     * @param type 视图类型
     */
    void SetType(ViewType type) { type_ = type; }

    /**
     * @brief 设置视口
     * @param viewport 视口描述符
     */
    void SetViewport(const rhi::ViewportDesc& viewport) { viewport_ = viewport; }

    /**
     * @brief 设置裁剪矩形
     * @param scissor 裁剪矩形
     */
    void SetScissor(const rhi::Rect& scissor) { scissor_ = scissor; }

    /**
     * @brief 设置阴影信息
     * @param info 阴影信息
     */
    void SetShadowInfo(const ShadowViewInfo& info) { shadowInfo_ = info; }

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
    ViewType GetType() const { return type_; }
    const rhi::ViewportDesc& GetViewport() const { return viewport_; }
    const rhi::Rect& GetScissor() const { return scissor_; }
    const ShadowViewInfo& GetShadowInfo() const { return shadowInfo_; }
    
    const math::m4x4& GetViewMatrix() const { return viewMatrix_; }
    const math::m4x4& GetProjectionMatrix() const { return projectionMatrix_; }
    const math::m4x4& GetViewProjectionMatrix() const { return viewProjectionMatrix_; }
    const rhi::Frustum& GetFrustum() const { return frustum_; }
    const utl::vector<const RenderProxy*>& GetVisibleProxies() const { return visibleProxies_; }

private:
    ViewType type_{ViewType::Main};
    rhi::ViewportDesc viewport_;
    rhi::Rect scissor_;
    ShadowViewInfo shadowInfo_;

    math::m4x4 viewMatrix_;
    math::m4x4 projectionMatrix_;
    math::m4x4 viewProjectionMatrix_;
    rhi::Frustum frustum_;

    utl::vector<const RenderProxy*> visibleProxies_;
};

} // namespace primal::graphics
