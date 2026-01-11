#pragma once
#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIMath.h"

namespace primal::graphics {

/**
 * @brief 渲染场景代理
 * @details 负责解耦 GamePlay ECS 与 渲染线程，持有渲染所需的变换和资源引用
 */
struct RenderProxy {
    math::m4x4 transform;       ///< 世界空间变换矩阵
    id::id_type meshId;         ///< 引用 RenderMesh 的 ID
    id::id_type materialId;     ///< 引用 MaterialInstance 的 ID
    id::id_type entityId;       ///< 对应的 GamePlay Entity ID

    RenderProxy();
    RenderProxy(id::id_type entity, id::id_type mesh, id::id_type material);

    /**
     * @brief 创建 RenderProxy 实例
     * @param entityId 实体ID
     * @param meshId 网格ID
     * @param materialId 材质ID
     * @return RenderProxy 实例
     */
    static RenderProxy Create(id::id_type entityId, id::id_type meshId, id::id_type materialId);

    /**
     * @brief 更新变换矩阵
     * @param newTransform 新的变换矩阵
     */
    void UpdateTransform(const math::m4x4& newTransform);

    /**
     * @brief 设置材质
     * @param newMaterialId 新的材质ID
     */
    void SetMaterial(id::id_type newMaterialId);
};

} // namespace primal::graphics
