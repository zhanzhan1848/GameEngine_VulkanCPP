#pragma once
#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include "Graphics/Material/ShaderTechnique.h"

namespace primal::graphics {

/**
 * @brief 渲染场景代理
 * @details 负责解耦 GamePlay ECS 与 渲染线程，持有渲染所需的变换和资源引用
 */
struct RenderProxy {
    math::m4x4 transform;       ///< 世界空间变换矩阵
    rhi::AABB worldAABB;        ///< 世界空间包围盒
    id::id_type meshId;         ///< 引用 RenderMesh 的 ID（编辑器管线路径 = mesh 资源
                                ///< 绑定的 ECS 实体 id，见 RegisterMeshEntity；SC
                                ///< cluster 门禁/RHISceneSynchronizer 按此键查）
    id::id_type contentMeshId{id::invalid_id}; ///< mesh 资产的 content geometry id
                                ///< （geometry_hierarchies 空间，可经
                                ///< content::get_rhi_mesh_id 两步解析到 RHI 资产）。
                                ///< 由 SyncEntitiesToRenderScene 从
                                ///< SceneDataMeshInfo::meshEntityId 回填；离线路径
                                ///< 两者同值。content 系统/烘焙收集按此字段解析，
                                ///< 勿用 meshId（2026-11-05 lightmap per-instance 排查）。
    id::id_type materialId;     ///< 引用 MaterialInstance 的 ID
    id::id_type entityId;       ///< 对应的 GamePlay Entity ID
    ShaderTechnique technique{ShaderTechnique::Opaque}; ///< 材质渲染技术类型
    f32 base_color[4]{1.f, 1.f, 1.f, 1.f};  ///< albedo tint
    f32 roughness{0.5f};                     ///< PBR roughness
    f32 metallic{0.0f};                      ///< PBR metallic
    f32 alpha_cutoff{0.5f};                  ///< alpha clip threshold
    f32 transmission{0.0f};                  ///< P2-B: 透光材质掩码（薄布料>0，石材=0）
    u64 graphVariant{0};                     ///< compiled material-graph content hash (0 = fixed material path)

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

private:
    /**
     * @brief 重新计算世界空间包围盒
     */
    void RecalculateWorldAABB();
};

} // namespace primal::graphics
