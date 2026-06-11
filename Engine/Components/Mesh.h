// 文件说明: Mesh 组件声明。负责实体与网格资源（几何/材质/LOD策略）的绑定与生命周期管理。
// 设计要点: 实例级绑定、与渲染项集成、支持 CPU→GPU 替换、LOD/策略更新的可扩展接口。

#pragma once

#include "ComponentsCommon.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/MeshComponent.h"

namespace primal::mesh {

    // 组件初始化信息
    struct init_info
    {
        // 函数说明: 初始几何资源 id（由 Content 创建的 geometry_content_id）。可为 invalid，后续通过 CPU 资产上传替换。
        id::id_type                 geometry_content_id{ id::invalid_id };
        // 函数说明: 材质 id 数组，与几何的 submesh 顺序一致
        const id::id_type*          material_ids{ nullptr };
        // 函数说明: 材质数量（也等于 submesh 数量）
        u32                         material_count{ 0 };
        // 函数说明: 是否在资产层保留 CPU 网格副本（用于优化与重上传）
        bool                        keep_cpu_copy{ true };
        // 函数说明: LOD 策略参数（偏置与强制层级）。-1 表示自动选择
        f32                         lod_bias{ 0.f };
        s32                         forced_lod{ -1 };
    };

    // 可选更新缓存（用于批量刷新策略或资源绑定）
    struct component_cache
    {
        mesh::mesh_id               id{ id::invalid_id };
        id::id_type                 geometry_content_id{ id::invalid_id };
        const id::id_type*          material_ids{ nullptr };
        u32                         material_count{ 0 };
        f32                         lod_bias{ 0.f };
        s32                         forced_lod{ -1 };
        u32                         flags{ 0 }; // bit0: 重新绑定几何; bit1: 更新LOD策略
    };

    // 函数说明: 创建 Mesh 组件并绑定渲染项
    mesh::component create(init_info info, game_entity::entity entity);
    // 函数说明: 移除 Mesh 组件并释放渲染项
    void remove(mesh::component c);
    // 函数说明: 用新的几何+材质重新绑定渲染项（支持 CPU→GPU 替换）
    void set_geometry(mesh::component c, id::id_type geometry_content_id,
                      const id::id_type* material_ids, u32 material_count);
    // 函数说明: 设置 LOD 策略参数
    void set_lod_policy(mesh::component c, f32 lod_bias, s32 forced_lod);
    // 函数说明: 批量更新 Mesh 组件缓存（便于系统层统一刷新）
    void update(const component_cache* caches, u32 count);
    // 函数说明: 获取 Mesh 组件的几何资源 id
    id::id_type get_geometry_id(component c);
}